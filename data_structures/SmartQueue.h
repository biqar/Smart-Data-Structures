#ifndef __SMART_QUEUE__
#define __SMART_QUEUE__

////////////////////////////////////////////////////////////////////////////////
// File    : SmartQueue.h
// Author  : Jonathan Eastep  email: eastep@mit.edu
// Written : 16 February 2011
//
// Copyright (C) 2011 Jonathan Eastep
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of 
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License 
// along with this program; if not, write to the Free Software Foundation
// Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
////////////////////////////////////////////////////////////////////////////////
// TODO:
//
////////////////////////////////////////////////////////////////////////////////

#include "cpp_framework.h"
#include "FCBase.h"
#include "LearningEngine.h"
#include "SmartLockLite.h"
#include "Heartbeat.h"
#include "Monitor.h"

#define _USE_SMARTLOCK
//#define _FC_CAS_STATS

using namespace CCP;

template <class T, bool _AUTO_TUNE = true, bool _AUTO_REWARD = true>
class SmartQueue : public FCBase<T> {
private:

        //constants -----------------------------------

        //inner classes -------------------------------
        struct Node {
                Node* volatile     _next;
                FCIntPtr volatile  _values[256];

                static Node* get_new(final int in_num_values) {
                        final size_t new_size = (sizeof(Node) + (in_num_values + 2 - 256) * sizeof(FCIntPtr));
                        
                        Node* final new_node = (Node*) malloc(new_size);
                        new_node->_next = null;
                        return new_node;
                }
        };

        //fields --------------------------------------
#ifdef _USE_SMARTLOCK
        SmartLockLite<FCIntPtr>*  _fc_lock         ATTRIBUTE_CACHE_ALIGNED;
#else
        AtomicInteger             _fc_lock;
#endif
        final int                 _NUM_REP;
        final int                 _REP_THRESHOLD;
        Monitor*                  _mon;
        LearningEngine*           _learner;
        int                       _sc_tune_id;

        Node* volatile            _head            ATTRIBUTE_CACHE_ALIGNED;
        Node* volatile            _tail;
        int volatile              _NODE_SIZE;
        Node* volatile            _new_node;
        int volatile              _size;
        _u64 volatile             _dead_count;
        bool volatile             _empty           ATTRIBUTE_CACHE_ALIGNED;
        char                      _pad             ATTRIBUTE_CACHE_ALIGNED;


        //helper function -----------------------------
        inline_ void flat_combining(final int iThread) {                

                // prepare for enq
                FCIntPtr volatile* enq_value_ary;
                if(null == _new_node) 
                        _new_node = Node::get_new(_NODE_SIZE);

                enq_value_ary = _new_node->_values;
                *enq_value_ary = 1;
                ++enq_value_ary;

                // prepare for deq
                FCIntPtr volatile * deq_value_ary = _tail->_values;
                deq_value_ary += deq_value_ary[0];

		++FCBase<T>::_cleanup_counter;
                int maxPasses;
                if ( !_AUTO_TUNE ) {
                        maxPasses = FCBase<T>::_num_passes;
			//std::cout << this << " num_passes " << maxPasses << std::endl;
		}
                else {
		        if ( 0 == (FCBase<T>::_cleanup_counter & 0xff) ) {
			        maxPasses = 1 + 10*_learner->samplediscval(_sc_tune_id);
                                //cout << "scancount = " << maxPasses << endl; 
			}
                        else 
                                maxPasses = 1 + 10*_learner->getdiscval(_sc_tune_id, iThread);
		}                

                int num_added = 0;
                int num_removed = 0;
                int total_changes = 0;

                for (int iTry=0;iTry<maxPasses; ++iTry) {
		        //test
                        //Memory::read_barrier();

                        int num_changes = 0;

                        SlotInfo* curr_slot = FCBase<T>::_tail_slot.get();
                        while(null != curr_slot->_next) {
                                final FCIntPtr curr_value = curr_slot->_req_ans;
                                if(curr_value > FCBase<T>::_NULL_VALUE) {
                                        if ( 0 == _gIsDedicatedMode )
                                                ++num_changes; 
                                        *enq_value_ary = curr_value;
                                        ++enq_value_ary;
                                        curr_slot->_req_ans = FCBase<T>::_NULL_VALUE;
                                        curr_slot->_time_stamp = FCBase<T>::_NULL_VALUE;

                                        ++num_added;
                                        if(num_added >= _NODE_SIZE) {
                                                Node* final new_node2 = Node::get_new(_NODE_SIZE+4);
                                                memcpy((void*)(new_node2->_values), (void*)(_new_node->_values), (_NODE_SIZE+2)*sizeof(FCIntPtr) );
                                                free(_new_node);
                                                _new_node = new_node2; 
                                                enq_value_ary = _new_node->_values;
                                                *enq_value_ary = 1;
                                                ++enq_value_ary;
                                                enq_value_ary += _NODE_SIZE;
                                                _NODE_SIZE += 4;
                                        }
                                } else if(FCBase<T>::_DEQ_VALUE == curr_value) {
				        if ( iTry == maxPasses-1 ) {
                                        final FCIntPtr curr_deq = *deq_value_ary;
                                        if(0 != curr_deq) {
                                                ++num_changes;
                                                ++num_removed;
                                                curr_slot->_req_ans = -curr_deq;
                                                curr_slot->_time_stamp = FCBase<T>::_NULL_VALUE;
                                                ++deq_value_ary;
                                        } else if(null != _tail->_next) {
                                                Node* tmp = _tail;
                                                _tail = _tail->_next;
                                                free(tmp);
                                                deq_value_ary = _tail->_values;
                                                deq_value_ary += deq_value_ary[0];
                                                continue;
                                        } else {
                                                if ( 0 == _gIsDedicatedMode )
                                                        ++num_changes;
                                                curr_slot->_req_ans = FCBase<T>::_NULL_VALUE;
                                                curr_slot->_time_stamp = FCBase<T>::_NULL_VALUE;
                                        } 
                                        }
                                }
                                curr_slot = curr_slot->_next;


                        }//while on slots

                        total_changes += num_changes;   
                        //if ( _AUTO_REWARD )
                        //        _mon->addreward(iThread, num_changes);

                }//for repetition

                _size += (num_added - num_removed);
                if ( _size == 0 && !_empty ) {
		        _empty = true;
                } else {
                        if (_empty)
		                 _empty = false;
		}
                                
                if ( (num_added==0) && (num_removed==0) )
		        _dead_count |= (U64(1) << iThread);
                else
		        _dead_count = 0;

                if ( _AUTO_REWARD )
                        _mon->addreward(iThread, total_changes);

                if(0 == *deq_value_ary && null != _tail->_next) {
                        Node* tmp = _tail;
                        _tail = _tail->_next;
                        free(tmp);
                } else {
                        _tail->_values[0] = (deq_value_ary -  _tail->_values);
                }

                if(enq_value_ary != (_new_node->_values + 1)) {
                        *enq_value_ary = 0;
                        _head->_next = _new_node;
                        _head = _new_node;
                        _new_node  = null;
                } 

        }

public:
        //public operations ---------------------------
        SmartQueue(Monitor* mon, LearningEngine* learner) 
        :       _NUM_REP(FCBase<T>::_NUM_THREADS),
                _REP_THRESHOLD((int)(Math::ceil(FCBase<T>::_NUM_THREADS/(1.7)))),
                _mon(mon),
                _learner(learner)
        {
	        assert( FCBase<T>::_NUM_THREADS <= (sizeof(_u64)*8) );
	        _size = 0;
                _empty = false;
                _dead_count = 0;
                _head = Node::get_new(FCBase<T>::_NUM_THREADS);
                _tail = _head;
                _head->_values[0] = 1;
                _head->_values[1] = 0;

                FCBase<T>::_timestamp = 0;
                _NODE_SIZE = 4;
                _new_node = null;

                _sc_tune_id = 0;
                if ( _AUTO_TUNE )
                        _sc_tune_id = _learner->register_sc_tune_id();

#ifdef _USE_SMARTLOCK
                _fc_lock = new SmartLockLite<FCIntPtr>(FCBase<T>::_NUM_THREADS, _learner);
#endif

                Memory::read_write_barrier();
        }

        virtual ~SmartQueue() 
        {
#ifdef _USE_SMARTLOCK
                delete _fc_lock;
#endif
        }

#ifdef _USE_SMARTLOCK
        //abort semaphore version
        //enq ......................................................
        boolean add(final int iThread, PtrNode<T>* final inPtr) {

                final FCIntPtr inValue = (FCIntPtr) inPtr;

                SlotInfo* my_slot = FCBase<T>::_tls_slot_info.get();
                if(null == my_slot)
                        my_slot = FCBase<T>::get_new_slot();

                SlotInfo* volatile& my_next   = my_slot->_next;
                FCIntPtr volatile*  my_re_ans = &my_slot->_req_ans;

                //test
		Memory::read_write_barrier();
                *my_re_ans = inValue;

                //this is needed because the combiner may remove you
                if (null == my_next)
                        FCBase<T>::enq_slot(my_slot);

                //Memory::write_barrier();
                boolean is_cas = _fc_lock->lock(my_re_ans, inValue, iThread);
                // when we get here, we either aborted or succeeded
                // abort happens when we got our answer
                if ( is_cas )
                {
                        // got the lock so we should do flat combining
                        CasInfo& my_cas_info = FCBase<T>::_cas_info_ary[iThread];
                        ++(my_cas_info._locks);
                        flat_combining(iThread);
                        _fc_lock->unlock(iThread);
                }

                //test
		//Memory::read_write_barrier();

                return true;
        }

        //deq ......................................................
        PtrNode<T>* remove(final int iThread, PtrNode<T>* final inPtr) {

                //test
		//Memory::read_write_barrier();

                final FCIntPtr inValue = (FCIntPtr) inPtr;

                SlotInfo* my_slot = FCBase<T>::_tls_slot_info.get();
                if(null == my_slot)
                        my_slot = FCBase<T>::get_new_slot();

                SlotInfo* volatile&     my_next = my_slot->_next;
                FCIntPtr volatile* my_re_ans = &my_slot->_req_ans;
                *my_re_ans = FCBase<T>::_DEQ_VALUE;

                //this is needed because the combiner may remove you
                if(null == my_next)
                        FCBase<T>::enq_slot(my_slot);

                //Memory::write_barrier();
                boolean is_cas = _fc_lock->lock(my_re_ans, FCBase<T>::_DEQ_VALUE, iThread);
                if( is_cas ) 
                {
                        CasInfo& my_cas_info = FCBase<T>::_cas_info_ary[iThread];
                        ++(my_cas_info._locks);                 
                        flat_combining(iThread);
                        _fc_lock->unlock(iThread);
                }
 
                //test
		//Memory::read_write_barrier();

                return (PtrNode<T>*) -(*my_re_ans);
        }
#else

        //enq ......................................................
        boolean add(final int iThread, PtrNode<T>* final inPtr) {
                final FCIntPtr inValue = (FCIntPtr) inPtr;
                CasInfo& my_cas_info = FCBase<T>::_cas_info_ary[iThread];

                //SlotInfo* my_slot = _tls_slot_info;
                SlotInfo* my_slot = FCBase<T>::_tls_slot_info.get();
                if(null == my_slot)
                        my_slot = FCBase<T>::get_new_slot();

                SlotInfo* volatile&  my_next   = my_slot->_next;
                FCIntPtr volatile*   my_re_ans = &my_slot->_req_ans;
                *my_re_ans = inValue;

                do {
                        //this is needed because the combiner may remove you
                        if (null == my_next)
                                FCBase<T>::enq_slot(my_slot);

                        boolean is_cas = true;
                        if(lock_fc(_fc_lock, is_cas)) {
#ifdef _FC_CAS_STATS
                                ++(my_cas_info._succ);
#endif
                                ++(my_cas_info._locks);
                                FCBase<T>::machine_start_fc(iThread);
                                flat_combining(iThread);
                                _fc_lock.set(0);
                                FCBase<T>::machine_end_fc(iThread);
#ifdef _FC_CAS_STATS
                                ++(my_cas_info._ops);
#endif
                                return true;
                        } else {
                                //Memory::write_barrier();
#ifdef _FC_CAS_STATS
                                if(!is_cas)
                                        ++(my_cas_info._failed);
#endif
                                while(FCBase<T>::_NULL_VALUE != *my_re_ans && 0 != _fc_lock.getNotSafe()) {
                                        FCBase<T>::thread_wait(iThread);
                                } 
                                //Memory::read_barrier();
                                if(FCBase<T>::_NULL_VALUE == *my_re_ans) {
#ifdef _FC_CAS_STATS
                                        ++(my_cas_info._ops);
#endif
                                        return true;
                                }
                        }
                } while(true);
        }

        //deq ......................................................
        PtrNode<T>* remove(final int iThread, PtrNode<T>* final inPtr) {
                final FCIntPtr inValue = (FCIntPtr) inPtr;
                CasInfo& my_cas_info = FCBase<T>::_cas_info_ary[iThread];

                //SlotInfo* my_slot = _tls_slot_info;
                SlotInfo* my_slot = FCBase<T>::_tls_slot_info.get();
                if(null == my_slot)
                        my_slot = FCBase<T>::get_new_slot();

                SlotInfo* volatile&     my_next = my_slot->_next;
                FCIntPtr volatile* my_re_ans = &my_slot->_req_ans;
                *my_re_ans = FCBase<T>::_DEQ_VALUE;

                do {
                        //this is needed because the combiner may remove you
                        if(null == my_next)
                                FCBase<T>::enq_slot(my_slot);

                        boolean is_cas = true;
                        if(lock_fc(_fc_lock, is_cas)) {
#ifdef _FC_CAS_STATS
                                ++(my_cas_info._succ);
#endif
                                ++(my_cas_info._locks);
                                FCBase<T>::machine_start_fc(iThread);
                                flat_combining(iThread);
                                _fc_lock.set(0);
                                FCBase<T>::machine_end_fc(iThread);
#ifdef _FC_CAS_STATS
                                ++(my_cas_info._ops);
#endif
                                return (PtrNode<T>*) -(*my_re_ans);
                        } else {
                                //Memory::write_barrier();
#ifdef _FC_CAS_STATS
                                if(!is_cas)
                                        ++(my_cas_info._failed);
#endif
                                while(FCBase<T>::_DEQ_VALUE == *my_re_ans && 0 != _fc_lock.getNotSafe()) {
                                        FCBase<T>::thread_wait(iThread);
                                }
                                //Memory::read_barrier();
                                if(FCBase<T>::_DEQ_VALUE != *my_re_ans) {
#ifdef _FC_CAS_STATS
                                        ++(my_cas_info._ops);
#endif
                                        return (PtrNode<T>*) -(*my_re_ans);
                                }
                        }
                } while(true);
        }


#endif

        //peek .....................................................
        PtrNode<T>* contain(final int iThread, PtrNode<T>* final inPtr) {
                final FCIntPtr inValue = (FCIntPtr) inPtr;
                return FCBase<T>::_NULL_VALUE;
        }

        //general .....................................................
        int size() {
                return _size;
        }

        final char* name() {
                return _AUTO_TUNE ? "SmartQueue" : "FCQueue";
        }

        bool dead() {
	        return _dead_count == ((U64(-1) >> (sizeof(_u64)*8 - FCBase<T>::_NUM_THREADS)));
        }

        bool empty() {
	        return _empty;
        }


        void cas_reset(final int iThread) {
#ifdef _USE_SMARTLOCK
                _fc_lock->resetcasops(iThread);
#endif
                FCBase<T>::_cas_info_ary[iThread].reset();
        }

        void print_custom() {
                int failed = 0;
                int succ = 0;
                int ops = 0;
                int locks = 0;

                for (int i=0; i<FCBase<T>::_NUM_THREADS; ++i) {
                        failed += FCBase<T>::_cas_info_ary[i]._failed;
                        succ += FCBase<T>::_cas_info_ary[i]._succ;
                        ops += FCBase<T>::_cas_info_ary[i]._ops;
                        locks += FCBase<T>::_cas_info_ary[i]._locks;
                }
#ifdef _USE_SMARTLOCK
                int tmp1 = _fc_lock->getcasops();
                int tmp2 = _fc_lock->getcasfails();
                succ += tmp1 - tmp2;
                failed += tmp2;
#endif
                printf(" 0 0 0 0 0 0 ( %d, %d, %d, %d, %d )", ops, locks, succ, failed, failed+succ);
        }

};


#endif
