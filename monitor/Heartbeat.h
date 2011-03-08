#ifndef __HEARTBEAT__
#define __HEARTBEAT__

////////////////////////////////////////////////////////////////////////////////
// File    : Heartbeat.h                                                        
// Authors : Jonathan Eastep   email: jonathan.eastep@gmail.com                 
//           Henry Hoffman     email: hank@.mit.edu                             
//           David Wingate     email: wingated@mit.edu                          
// Written : 16 February 2011                                                   
//                                                                              
// A much simplified version of Heartbeats                                    
//                                                                              
// Copyright (C) 2011 Jonathan Eastep, Henry Hoffman, David Wingate             
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

#include "portable_defns.h"
#include "cpp_framework.h"
#include "Monitor.h"
#include "FCBase.h"


class Hb: public Monitor, public FCBase<FCIntPtr> {

private:

        //mode. non-concurrent mode enables optimization
        final bool         concurrent     ATTRIBUTE_CACHE_ALIGNED;

        //written atomically: via write in !concurrent mode otherwise via atomic ops 
        volatile _u64      total_items    ATTRIBUTE_CACHE_ALIGNED; 

        //backoff settings in nanoseconds
        static final _u64  BACKOFF_START  ATTRIBUTE_CACHE_ALIGNED  = 100;
        static final _u64  BACKOFF_MAX                             = 1600;

        char               pad            ATTRIBUTE_CACHE_ALIGNED;

public:

        //---------------------------------------------------------------------------
        // Heartbeats API
        //--------------------------------------------------------------------------- 

        Hb(bool concurrent = true)
	: concurrent(concurrent),
	  total_items(0)
        {
                CCP::Memory::read_write_barrier();
        }

        ~Hb() {}

        inline _u64 waitheartbeatsnotsafe(_u64 lastval)
	{
                //check if changed. if so return new val.
                _u64 newval = total_items;
                if ( newval != lastval )
                        return newval;

                //spin with backoff until val changes
                _u64 backoff = BACKOFF_START; 
                do {
                        CCP::Thread::delay(backoff);
                        backoff <<= 1;
                        if ( backoff > BACKOFF_MAX )
                                return total_items;

                        newval = total_items;
                } while( newval == lastval );

                //return new val
                return newval;
	}

        inline _u64 spinheartbeatsnotsafe(_u64 lastval)
	{
                _u64 newval;
                do {
                        newval = total_items;
                } while( newval == lastval );

                return newval;
	}

        inline _u64 readheartbeatsnotsafe() {
	        return total_items;
        }

        inline _u64 readheartbeats() {
	        return concurrent ? FAADD(&total_items, 0) : total_items;  
        }

        inline void heartbeatnotsafe(int num_beats = 1) {
	        total_items += num_beats;
        }  

        inline void heartbeat(int num_beats = 1) {
	        if ( concurrent )
                        _u64 tmp = FAADD(&total_items, num_beats);     
                else 
		        total_items += num_beats;
        }
        

        //---------------------------------------------------------------------------
        // Monitor API
        //---------------------------------------------------------------------------

        inline _u64 waitrewardnotsafe(_u64 lastval) { return waitheartbeatsnotsafe(lastval); }

        inline _u64 getrewardnotsafe() { return readheartbeatsnotsafe(); }

        inline _u64 getreward() { return readheartbeats(); }

        inline void addrewardnotsafe(int tid, _u64 amt) { heartbeatnotsafe(amt); }

        inline void addreward(int tid, _u64 amt) { heartbeat(amt); }


        //---------------------------------------------------------------------------
        // Hacky Benchmark API
        //---------------------------------------------------------------------------

        boolean add(final int iThread, PtrNode<FCIntPtr>* final inPtr) {
	        addreward(iThread, 1);
                return true;
	}

        PtrNode<FCIntPtr>* remove(final int iThread, PtrNode<FCIntPtr>* final inPtr) {
	        addreward(iThread, U64(-1));
                return (PtrNode<FCIntPtr>*) 1;
	}

        PtrNode<FCIntPtr>* contain(final int iThread, PtrNode<FCIntPtr>* final inPtr) {
	        _u64 rv = waitrewardnotsafe( (_u64) inPtr );
                return (PtrNode<FCIntPtr>*) rv;
	}    

        int size() {
                return 0;
        }

        final char* name() {
                return "heartbeat";
        }


};


#endif
