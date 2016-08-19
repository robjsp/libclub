// Copyright 2016 Peter Jankuliak
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef CLUB_TRANSPORT_CORE_H
#define CLUB_TRANSPORT_CORE_H

#include <map>
#include "transport/in_message_part.h"
#include "transport/in_message_full.h"
#include "transport/out_message.h"
#include "transport/ack_set.h"
#include "transport/ack_entry.h"
#include "transport/outbound_acks.h"
#include "transport/pending_message.h"
#include "transport/dijkstra.h"

#include <club/debug/ostream_uuid.h>
#include "debug/string_tools.h"
#include "message_id.h"

namespace club { namespace transport {

template<typename> class TransmitQueue;
template<typename> class Relay;

template<typename UnreliableId>
class Core {
private:
  using OnReceive  = std::function<void(uuid, boost::asio::const_buffer)>;
  using OnFlush    = std::function<void()>;
  using Queue      = TransmitQueue<UnreliableId>;
  using OutMessage = transport::OutMessage;
  using Relay      = transport::Relay<UnreliableId>;
  using MessageId  = transport::MessageId<UnreliableId>;
  using Messages   = std::map<MessageId, std::weak_ptr<OutMessage>>;

  using UnreliableBroadcastId = transport::UnreliableBroadcastId<UnreliableId>;


public:
  Core(uuid our_id, OnReceive);

  void broadcast_reliable(std::vector<uint8_t>&& data);

  void broadcast_unreliable( UnreliableId           id
                           , std::vector<uint8_t>&& data
                           , std::set<uuid>         targets);

  void broadcast_unreliable( UnreliableId           id
                           , std::vector<uint8_t>&& data);

  const uuid& id() const { return _our_id; }

  void reset_topology(const Graph<uuid>&);

  void flush(OnFlush);

  ~Core();

private:
  friend class ::club::transport::Relay<UnreliableId>;
  friend class ::club::transport::TransmitQueue<UnreliableId>;

  void release(MessageId, std::shared_ptr<OutMessage>&&);

  void register_relay(Relay*);
  void unregister_relay(Relay*);
  void forward_message(const InMessagePart&);

  void add_ack_entry(AckEntry);
  uint8_t encode_acks(binary::encoder& encoder, const std::set<uuid>& targets);

  void add_target_to_transport(Relay&, uuid);

  void on_receive_part(InMessagePart);
  void on_receive_full(InMessageFull);

  void on_receive_acks(const uuid&, AckSet);
  void acknowledge(const uuid&, AckSet::Type, SequenceNumber);
  void acknowledge(const InMessageFull&);

  void try_flush();

private:
  using PendingMessages = std::map<SequenceNumber, PendingMessage>;

  struct Target {
    struct Sync {
      SequenceNumber last_executed_message;
      AckSet         acks;
    };

    boost::optional<Sync> sync;
    PendingMessages       pending;
  };

  PendingMessage& add_to_pending(Target&, InMessagePart&&);
  PendingMessage& add_to_pending(Target&, InMessageFull&&);

  void replay_pending_messages(Target&);

  template<class Key, class Value>
  static
  std::set<Key> keys(const std::map<Key, Value>& map) {
    std::set<Key> retval;
    for (const auto& pair : map)
      retval.insert(retval.end(), pair.first);
    return retval;
  }

private:
  uuid                   _our_id;
  OnReceive              _on_recv;
  SequenceNumber         _next_reliable_broadcast_number;
  // This number should be unique for each packet sent, i.e. even
  // a particular message - if sent multiple times - should always
  // have this number incremented.
  // TODO: The above currently doesn't hold.
  SequenceNumber         _next_message_number;
  std::set<Relay*>       _relays;
  Messages               _messages;
  OnFlush                _on_flush;

  std::map<uuid, Target>  _targets;
  OutboundAcks            _outbound_acks;

  std::shared_ptr<bool> _was_destroyed;
};

//------------------------------------------------------------------------------
// Implementation
//------------------------------------------------------------------------------
template<class Id> Core<Id>::Core(uuid our_id, OnReceive on_recv)
  : _our_id(std::move(our_id))
  , _on_recv(std::move(on_recv))
  , _next_reliable_broadcast_number(0) // TODO: Should this be initialized to a random number?
  , _next_message_number(0)
  , _outbound_acks(_our_id)
  , _was_destroyed(std::make_shared<bool>(false))
{
}

//------------------------------------------------------------------------------
template<class Id> Core<Id>::~Core() {
  *_was_destroyed = true;
}
//------------------------------------------------------------------------------
template<class Id> void Core<Id>::register_relay(Relay* t)
{
  _relays.insert(t);
}

//------------------------------------------------------------------------------
template<class Id> void Core<Id>::unregister_relay(Relay* t)
{
  _relays.erase(t);
}

//------------------------------------------------------------------------------
template<class Id>
void Core<Id>::reset_topology(const Graph<uuid>& graph) {
  for (auto r : _relays) {
    r->_targets.clear();
  }

  // TODO: Big one: we only assign one relay per target, but there could
  // be more than one 'shortest' path to the target which could allow bigger
  // message throughput.
  Dijkstra dijkstra(_our_id, graph);

  // TODO: The following has terrible complexity
  auto find_transport = [&](const uuid& id) -> Relay* {
    for (auto r : _relays) if (r->_relay_id == id) return r;
    return nullptr;
  };

  for (const auto& target : graph.nodes) {
    if (auto opt_transport = dijkstra.first_node_to(target)) {
      if (auto transport = find_transport(*opt_transport)) {
        add_target_to_transport(*transport, target);
      }
    }
  }
}

//------------------------------------------------------------------------------
template<class Id>
uint8_t Core<Id>::encode_acks( binary::encoder& encoder
                             , const std::set<uuid>& targets) {
  return _outbound_acks.encode_few(encoder, targets);
}

//------------------------------------------------------------------------------
template<class Id>
void Core<Id>::add_ack_entry(AckEntry entry) {
  _outbound_acks.add_ack_entry(std::move(entry));
}

//------------------------------------------------------------------------------
template<class Id>
void Core<Id>::acknowledge(const InMessageFull& msg) {
  AckSet::Type type;

  switch (msg.type) {
    case MessageType::reliable_broadcast: type = AckSet::Type::broadcast; break;
    case MessageType::syn:                type = AckSet::Type::unicast; break;
    default:
                                          assert(0);
                                          return;
  }

  _outbound_acks.acknowledge(msg.source, type, msg.sequence_number);
}

//------------------------------------------------------------------------------
template<class Id>
void Core<Id>::on_receive_acks(const uuid& target, AckSet acks) {
  bool acked_some = false;

  for (auto sn : acks) {
    MessageId mid;

    switch (acks.type()) {
      case AckSet::Type::unicast:   mid = ReliableUnicastId{target, sn}; break;
      case AckSet::Type::broadcast: mid = ReliableBroadcastId{sn};       break;
      default: assert(0); return;
    }

    auto i = _messages.find(mid);

    if (i != _messages.end()) {
      if (auto message = i->second.lock()) {
        message->targets.erase(target);

        if (message->targets.empty()) {
          _messages.erase(i);
        }

        acked_some = true;
      }
    }
  }

  if (acked_some) {
    try_flush();
  }
}

//------------------------------------------------------------------------------
template<class Id>
void
Core<Id>::broadcast_reliable(std::vector<uint8_t>&& data) {
  using namespace std;

  auto sn = _next_reliable_broadcast_number++;

  auto type = MessageType::reliable_broadcast;

  auto message = make_shared<OutMessage>( _our_id
                                        , keys(_targets)
                                        , true
                                        , type
                                        , sn
                                        , move(data)
                                        );

  ReliableBroadcastId id{sn};

  _messages.emplace(id, message);

  for (auto r : _relays) {
    r->insert_message(id, message);
  }
}

//------------------------------------------------------------------------------
template<class Id>
void Core<Id>::add_target_to_transport(Relay& relay, uuid new_target) {
  using namespace std;

  if (!relay.add_target(new_target)) return;

  auto inserted = _targets.emplace(new_target, Target()).second;

  if (inserted) {
    auto sn = _next_reliable_broadcast_number;

    auto message = make_shared<OutMessage>( _our_id
                                          , set<uuid>{new_target}
                                          , true
                                          , MessageType::syn
                                          , sn
                                          , std::vector<uint8_t>());

    ReliableUnicastId id{std::move(new_target), sn};

    _messages.emplace(id, message);

    for (auto r : _relays) {
      r->insert_message(id, message);
    }
  }
  else {
    // The target was already there, byt a different relay is/was sending to
    // it. The other relay may soon remove the target from its list so
    // we need to take care the message gets delivered.
    for (auto& m : _messages) {
      auto m_ptr = m.second.lock();
      if (!m_ptr) continue;
      if (m_ptr->targets.count(new_target)) {
        relay.insert_message(m.first, m_ptr);
      }
    }
  }
}

//------------------------------------------------------------------------------
template<class Id>
void Core<Id>::broadcast_unreliable( Id                     id
                                   , std::vector<uint8_t>&& data) {
  using namespace std;

  auto i = _messages.find(UnreliableBroadcastId{id});

  if (i != _messages.end()) {
    if (auto p = i->second.lock()) {
      p->reset_payload(std::move(data));
    }
    // else { it was there but was already sent, so noop }
  }
  else {
    auto sn   = _next_message_number++;
    auto type = MessageType::unreliable_broadcast;

    auto message = make_shared<OutMessage>( _our_id
                                          , keys(_targets)
                                          , false
                                          , type
                                          , sn
                                          , move(data)
                                          );

    UnreliableBroadcastId mid{id};

    _messages.emplace(mid, move(message));

    for (auto r : _relays) {
      r->insert_message(mid, message);
    }
  }

}

//------------------------------------------------------------------------------
template<class Id>
void Core<Id>::forward_message(const InMessagePart& msg) {
  using namespace std;

  auto begin = boost::asio::buffer_cast<const uint8_t*>(msg.type_and_payload);
  auto size  = boost::asio::buffer_size(msg.type_and_payload);

  std::vector<uint8_t> data(begin, begin + size);

  auto message = make_shared<OutMessage>( msg.source
                                        , set<uuid>(msg.targets)
                                        , false
                                        //, msg.type
                                        //, msg.sequence_number
                                        , move(data) );

  // TODO: Same as with unreliable messages, store the message in a
  // std::map so that we don't put identical messages to message queues
  // more than once.

  ForwardId id;

  for (auto r : _relays) {
    r->insert_message(id, message);
  }
}

//------------------------------------------------------------------------------
// TODO: Reduce these two functions that do almost exactly the same thing.
template<class Id>
PendingMessage& Core<Id>::add_to_pending(Target& node, InMessagePart&& msg) {
  auto pending_i = node.pending.find(msg.sequence_number);

  if (pending_i == node.pending.end()) {
    auto pi = node.pending.emplace(msg.sequence_number, PendingMessage(std::move(msg)))
      .first;
    return pi->second;
  }
  else {
    auto& pm = pending_i->second;
    pm.update_payload(msg.chunk_start, msg.payload);
    return pm;
  }
}

template<class Id>
PendingMessage& Core<Id>::add_to_pending(Target& node, InMessageFull&& msg) {
  auto pending_i = node.pending.find(msg.sequence_number);

  if (pending_i == node.pending.end()) {
    auto pi = node.pending.emplace(msg.sequence_number, PendingMessage(std::move(msg)))
      .first;
    return pi->second;
  }
  else {
    auto& pm = pending_i->second;
    pm.update_payload(0, msg.payload);
    return pm;
  }
}

//------------------------------------------------------------------------------
template<class Id>
void Core<Id>::on_receive_part(InMessagePart msg) {
  if (msg.is_full()) {
    on_receive_full(InMessageFull( msg.source
                                 , msg.type
                                 , msg.sequence_number
                                 , msg.original_size
                                 , msg.payload));
    return;
  }

  if (msg.type != MessageType::reliable_broadcast
      && msg.type != MessageType::unreliable_broadcast) return;

  auto i = _targets.find(msg.source);
  if (i == _targets.end()) return;
  auto& node = i->second;
  if (!node.sync) return;
  if (!node.sync->acks.can_add(msg.sequence_number)) return;

  auto& pm = add_to_pending(node, std::move(msg));

  if (auto opt_full_msg = pm.get_full_message()) {
    on_receive_full(std::move(*opt_full_msg));
  }
}

//------------------------------------------------------------------------------
template<class Id>
void Core<Id>::on_receive_full(InMessageFull msg) {
  using std::move;

  using namespace boost::asio;
  auto i = _targets.find(msg.source);

  // If there is no Target for this source we have not yet
  // attempted to establish connection with it.
  // (User has not called `add_target`).
  if (i == _targets.end()) {
    return;
  }

  auto &node = i->second;

  if (msg.type == MessageType::reliable_broadcast) {
    // Have we received syn packet yet?
    if (!node.sync) return;

    // If the remote peer is sending too quickly we refuse to receive
    // and acknowledge the message.
    if (node.sync->acks.try_add(msg.sequence_number)) {
      acknowledge(msg);

      if (msg.sequence_number == node.sync->last_executed_message + 1) {
        auto was_destroyed = _was_destroyed;

        // TODO: Is OK for invokation of _on_recv to destroy _on_recv?
        node.sync->last_executed_message = msg.sequence_number;
        _on_recv(msg.source, msg.payload);
        if (*was_destroyed) return;

        // This should be the last thing this function does (or you need
        // to check the was_destroyed flag again).
        replay_pending_messages(node);
      }
      else if (msg.sequence_number > node.sync->last_executed_message + 1) {
        add_to_pending(node, move(msg));
      }
    }
  }
  else if (msg.type == MessageType::unreliable_broadcast) {
    // Have we received syn packet yet?
    if (!node.sync) return;

    _on_recv(msg.source, msg.payload);
  }
  else if (msg.type == MessageType::syn) {
    acknowledge(msg);

    if (!node.sync) {
      node.sync = typename Target::Sync{ msg.sequence_number-1
                                       , AckSet( AckSet::Type::broadcast
                                               , msg.sequence_number-1)};

      // No need to replay pending messages here because, we've been ignoring
      // everything until now.
    }
  }
  else {
    // TODO: Disconnect from the sender.
    assert(0);
  }
}

//------------------------------------------------------------------------------
template<class Id>
void Core<Id>::replay_pending_messages(Target& node) {
  auto was_destroyed = _was_destroyed;

  for (auto i = node.pending.begin(); i != node.pending.end();) {
    if (i->first <= node.sync->last_executed_message) {
      i = node.pending.erase(i);
      continue;
    }

    auto next_sn = node.sync->last_executed_message + 1;

    if (i->first != next_sn) break;

    if (auto opt_full_message = i->second.get_full_message())
    {
      auto& msg = *opt_full_message;

      acknowledge(msg);

      _on_recv(msg.source, msg.payload);

      if (*was_destroyed) return;

      node.sync->last_executed_message = next_sn;

      i = node.pending.erase(i);
    }
    else {
      break;
    }
  }
}

//------------------------------------------------------------------------------
template<class Id>
void Core<Id>::release( MessageId message_id
                      , std::shared_ptr<OutMessage>&& m) {
  // For reliable messages, we only treat as reliable those that originated
  // here. Also, we don't store unreliable messages that did not originate
  // here in _messages because we don't want this user to change
  // them anyway.
  if (m->source != _our_id) return;

  auto i = _messages.find(message_id);
  if (i == _messages.end()) return;
  if (m.use_count() > 1) return; // Someone else still uses this message.

  // TODO: In case of reliable messages, if the `targets` variable of the
  // message is not empty, we must store it to some other collection (could be
  // called `_orphans`) and remove it from there when we're notified that a
  // node was removed from the network.

  _messages.erase(i);
}

//------------------------------------------------------------------------------
template<class Id>
void Core<Id>::flush(OnFlush on_flush) {
  _on_flush = std::move(on_flush);
}

//------------------------------------------------------------------------------
template<class Id> void Core<Id>::try_flush() {
  if (!_on_flush) return;

  // TODO: We should probably also check that all acks have been sent.

  if (!_messages.empty()) {
    return;
  }

  // TODO: Transports could increment and decrement some counter when sending
  // and finishing sending so that we wouldn't have to iterate here through
  // the transports.
  for (auto r : _relays) {
    if (r->is_sending()) return;
  }

  auto on_flush = std::move(_on_flush);
  on_flush();
}

//------------------------------------------------------------------------------
}} // club::transport namespace

#endif // ifndef CLUB_TRANSPORT_CORE_H
