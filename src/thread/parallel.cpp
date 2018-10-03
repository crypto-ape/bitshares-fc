/*
 * Copyright (c) 2018 The BitShares Blockchain, and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <fc/thread/parallel.hpp>
#include <fc/thread/spin_yield_lock.hpp>
#include <fc/thread/unique_lock.hpp>
#include <fc/asio.hpp>

#include <boost/atomic/atomic.hpp>
#include <boost/lockfree/queue.hpp>

namespace fc {
   namespace detail {
      class pool_impl;

      class idle_notifier_impl : public thread_idle_notifier
      {
      public:
         idle_notifier_impl()
         {
            is_idle.store(false);
         }

         idle_notifier_impl( const idle_notifier_impl& copy )
         {
            id = copy.id;
            my_pool = copy.my_pool;
            is_idle.store( copy.is_idle.load() );
         }

         virtual task_base* idle();
         virtual void       busy()
         {
            is_idle.store(false);
         }

         uint32_t            id;
         pool_impl*          my_pool;
         boost::atomic<bool> is_idle;
      };

      class pool_impl
      {
      public:
         pool_impl( const uint16_t num_threads )
         {
            notifiers.resize( num_threads );
            threads.reserve( num_threads );
            for( uint32_t i = 0; i < num_threads; i++ )
            {
               notifiers[i].id = i;
               notifiers[i].my_pool = this;
               threads.push_back( new thread( "pool worker " + fc::to_string(i), &notifiers[i] ) );
            }
         }
         ~pool_impl()
         {
            for( thread* t : threads)
               delete t; // also calls quit()
            waiting_tasks.consume_all( [] ( task_base* t ) {
               t->cancel( "thread pool quitting" );
            });
         }

         void post( task_base* task )
         {
            idle_notifier_impl* ini;
            while( idle_threads.pop( ini ) )
               if( ini->is_idle.exchange( false ) )
               { // minor race condition here, a thread might receive a task while it's busy
                  threads[ini->id]->async_task( task, priority() );
                  return;
               }
            boost::unique_lock<fc::spin_yield_lock> lock(pool_lock);
            while( idle_threads.pop( ini ) )
               if( ini->is_idle.exchange( false ) )
               { // minor race condition here, a thread might receive a task while it's busy
                  threads[ini->id]->async_task( task, priority() );
                  return;
               }
            waiting_tasks.push( task );
         }

         task_base* enqueue_idle_thread( idle_notifier_impl* ini )
         {
            task_base* task;
            if( waiting_tasks.pop( task ) )
               return task;
            fc::unique_lock<fc::spin_yield_lock> lock(pool_lock);
            if( waiting_tasks.pop( task ) )
               return task;
            idle_threads.push( ini );
            return 0;
         }
      private:
         std::vector<idle_notifier_impl>                notifiers;
         std::vector<thread*>                           threads;
         boost::lockfree::queue<idle_notifier_impl*>    idle_threads;
         boost::lockfree::queue<task_base*>             waiting_tasks;
         fc::spin_yield_lock                            pool_lock;
      };

      task_base* idle_notifier_impl::idle()
      {
         is_idle.store( true );
         task_base* result = my_pool->enqueue_idle_thread( this );
         if( result ) is_idle.store( false );
         return result;
      }

      worker_pool::worker_pool()
      {
         fc::asio::default_io_service();
         my = new pool_impl( fc::asio::default_io_service_scope::get_num_threads() );
      }

      worker_pool::~worker_pool()
      {
         delete my;
      }

      void worker_pool::post( task_base* task )
      {
         my->post( task );
      }

      worker_pool& get_worker_pool()
      {
         static worker_pool the_pool;
         return the_pool;
      }
   }
}