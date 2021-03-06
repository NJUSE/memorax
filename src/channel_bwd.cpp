/*
 * Copyright (C) 2012 Carl Leonardsson
 * 
 * This file is part of Memorax.
 *
 * Memorax is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * Memorax is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
 * License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "channel_bwd.h"

Trace *ChannelBwd::convert_trace(Trace *trace, ChannelConstraint::Common *common) const{

  /* Check the order of writes */
  /* For each writing instruction w in trace, writes contains an entry
   * (i,m) where i is the index of w in trace and m is the
   * corresponding SB channel message.
   *
   * writes is ordered w.r.t. the indices.
   */
  std::vector<std::pair<int,ChannelConstraint::Msg> > writes;
  for(int i = 1; i <= trace->size(); ++i){
    if(produces_message(trace->transition(i)->instruction)){
      if(trace->constraint(i) == 0){
        throw new std::logic_error("ChannelBwd::convert_trace: Trace is incomplete: Missing constraints.");
      }
      assert(dynamic_cast<const ChannelConstraint*>(trace->constraint(i)));
      const ChannelConstraint *chc = static_cast<const ChannelConstraint*>(trace->constraint(i));
      writes.push_back(std::pair<int,ChannelConstraint::Msg>(i,chc->channel[chc->channel.size()-1]));
    }
  }

  /* Associate updates with writes */
  std::map<std::pair<int,int>,int> write_to_update; // write_to_update[(p,w)] is the update of process p corresponding to write w
  {
    std::vector<int> channel; // channel[i] is the index of the write producing message i
    std::vector<int> proc_seen_until(common->machine.automata.size(),-1); // Pointer into writes
    channel.push_back(-1); // The dummy message
    for(int i = 1; i <= trace->size(); ++i){
      if(trace->constraint(i-1) == 0 || trace->constraint(i) == 0){
        throw new std::logic_error("ChannelBwd::convert_trace: Trace is incomplete: Missing constraints.");
      }
      assert(dynamic_cast<const ChannelConstraint*>(trace->constraint(i-1)));
      assert(dynamic_cast<const ChannelConstraint*>(trace->constraint(i)));
      const ChannelConstraint *chc0 = static_cast<const ChannelConstraint*>(trace->constraint(i-1));
      const ChannelConstraint *chc1 = static_cast<const ChannelConstraint*>(trace->constraint(i));
      if(produces_message(trace->transition(i)->instruction)){
        if(chc0->channel.size() + 1 == chc1->channel.size()){
          /* No messages lost */
          channel.push_back(i);
        }else{
          assert(chc1->channel.size() <= chc0->channel.size());
          /* Messages lost */
          messages_lost(chc0->channel, chc1->channel, &channel, i, common);
        }
      }else if(consumes_message(trace->transition(i)->instruction) &&
               chc1->channel.size() != chc0->channel.size()){
          assert(chc1->channel.size() == chc0->channel.size()-1);
          channel.erase(channel.begin());
      }
      if(consumes_message(trace->transition(i)->instruction)){
        int pid = trace->transition(i)->pid;
        int w = proc_seen_until[pid] + 1;
        int tgt_w = channel[chc1->cpointers[pid]];
        while(writes[w].first != tgt_w){
          Log::debug << "Lost message (for process " << pid << "): "
                     << trace->transition(writes[w].first)->to_string(common->machine)
                     << "\n";
          write_to_update[std::pair<int,int>(pid,writes[w].first)] = i;
          ++w;
        }
        assert(trace->transition(i)->instruction.get_type() == Lang::UPDATE ||
               i == writes[w].first);
        write_to_update[std::pair<int,int>(pid,writes[w].first)] = i;
        proc_seen_until[pid] = w;
      }
      assert(channel.size() == chc1->channel.size());
    }
#ifndef NDEBUG
    for(unsigned p = 0; p < common->machine.automata.size(); ++p){
      assert(proc_seen_until[p] == int(writes.size())-1);
    }
#endif
  }

  /* Produce a TSO trace */

  /* For each write produce first the section of the TSO trace that
   * preceeds the corresponding update. */
  Trace *tso_trace = new Trace(0);
  std::vector<int> proc_pos(common->machine.automata.size(),1);
  for(unsigned w = 0; w <= writes.size(); ++w){
    /* Run all processes up to the point where they update w.r.t. writes[w] */
    for(unsigned p = 0; p < common->machine.automata.size(); ++p){
      while((w == writes.size() && proc_pos[p] <= trace->size()) ||
            (w < writes.size() && write_to_update[std::pair<int,int>(p,writes[w].first)] != proc_pos[p])){
        if(trace->transition(proc_pos[p])->pid == int(p) &&
           !consumes_message(trace->transition(proc_pos[p])->instruction)){
          tso_trace->push_back(*trace->transition(proc_pos[p]),0);
        }
        ++proc_pos[p];
      }
    }
    if(w < writes.size()){
      /* Perform the update */
      if((*trace)[writes[w].first]->instruction.get_type() == Lang::LOCKED){
        /* locked */
        assert(((write_to_update[std::pair<int,int>(writes[w].second.wpid,writes[w].first)]) == writes[w].first));
        tso_trace->push_back(*(*trace)[writes[w].first],0);
      }else{
        /* ordinary write,update */
        int wpid = writes[w].second.wpid;
        int uindex = write_to_update[std::pair<int,int>(wpid,writes[w].first)];
        assert((*trace)[uindex]->instruction.get_type() == Lang::UPDATE);
        int q = (*trace)[uindex]->source;
        VecSet<Lang::MemLoc<int> > mls;
        for(Lang::NML nml : writes[w].second.nmls){
          mls.insert(nml.localize(wpid));
        }
        tso_trace->push_back({q,Lang::Stmt<int>::update(wpid,mls),q,wpid},0);
      }
    }
  }

  return tso_trace;
};

void ChannelBwd::messages_lost(const std::vector<ChannelConstraint::Msg> &ch0,
                               const std::vector<ChannelConstraint::Msg> &ch1,
                               std::vector<int> *ch,
                               int w,
                               const ChannelConstraint::Common *common) const{
  assert(ch1.size() <= ch0.size());
  VecSet<int> to_remove; // Indices into ch
  /* For each possible message msg, check if an instance if that message has
   * been lost. */
  for(const ChannelConstraint::Common::MsgHdr msg : common->messages){
    /* Count the number of occurrences of msg in ch0 and ch1 */
    int ch0_count = 0;
    int ch1_count = 0;
    int ch0_rmi = -1; // Rightmost index of msg in ch0
    for(unsigned i = 0; i < ch0.size(); ++i){
      if(ch0[i].wpid == msg.wpid && ch0[i].nmls == msg.nmls){
        ++ch0_count;
        ch0_rmi = i;
      }
    }
    for(unsigned i = 0; i < ch1.size(); ++i){
      if(ch1[i].wpid == msg.wpid && ch1[i].nmls == msg.nmls){
        ++ch1_count;
      }
    }
    bool is_w_msg =
      (ch1.back().wpid == msg.wpid && ch1.back().nmls == msg.nmls);
    /* Has a message corresponding to msg been lost? */
    bool is_lost;
    if(is_w_msg){
      is_lost = (ch1_count <= ch0_count);
      if(is_lost) assert(ch1_count == ch0_count);
    }else{
      is_lost = (ch1_count < ch0_count);
      if(is_lost) assert(ch1_count == ch0_count - 1);
    }

    if (is_lost){
      /* The lost message is the rightmost occurrence of msg in ch0. */
      assert(ch0_rmi != -1);
      to_remove.insert(ch0_rmi);
    }
  }

  /* Remove messages in to_remove */
  int j = 0;
  for(unsigned i = 0; i < ch->size(); ++i){
    if(to_remove.count(i) > 0){
      // Do nothing
    }else{
      (*ch)[j] = (*ch)[i];
      ++j;
    }
  }
  assert(j + int(to_remove.size()) == int(ch->size()));
  ch->resize(j);

  ch->push_back(w);
};
