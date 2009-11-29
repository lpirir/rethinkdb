
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <strings.h>
#include <new>
#include "config.hpp"
#include "utils.hpp"
#include "event_queue.hpp"

// TODO: report event queue statistics.

void process_aio_notify(event_queue_t *self) {
    int res, nevents;
    eventfd_t nevents_total;
    res = eventfd_read(self->aio_notify_fd, &nevents_total);
    check("Could not read aio_notify_fd value", res != 0);

    // Note: O(1) array allocators are hard. To avoid all the
    // complexity, we'll use a fixed sized array and call io_getevents
    // multiple times if we have to (which should be very unlikely,
    // anyway).
    io_event events[MAX_IO_EVENT_PROCESSING_BATCH_SIZE];
    
    do {
        // Grab the events
        nevents = io_getevents(self->aio_context, 1, MAX_IO_EVENT_PROCESSING_BATCH_SIZE,
                               events, NULL);
        check("Waiting for AIO event failed", nevents < 1);
        
        // Process the events
        for(int i = 0; i < nevents; i++) {
            if(self->event_handler) {
                event_t qevent;
                bzero((char*)&qevent, sizeof(qevent));
                qevent.event_type = et_disk_event;
                iocb *op = (iocb*)events[i].obj;
                qevent.source = op->aio_fildes;
                qevent.result = events[i].res;
                qevent.buf = op->u.c.buf;
                qevent.state = events[i].data;
                if(op->aio_lio_opcode == IO_CMD_PREAD)
                    qevent.op = eo_read;
                else
                    qevent.op = eo_write;
                self->event_handler(self, &qevent);
            }
            self->alloc.free(events[i].obj);
        }
        nevents_total -= nevents;
    } while(nevents_total > 0);
}

void process_timer_notify(event_queue_t *self) {
    int res;
    eventfd_t nexpirations;
    res = eventfd_read(self->timer_fd, &nexpirations);
    check("Could not read timer_fd value", res != 0);

    self->total_expirations += nexpirations;

    // Internal ops to perform on the timer
    if(self->total_expirations % ALLOC_GC_IN_TICKS == 0) {
        // Perform allocator gc
        self->alloc.gc();
    }

    // Let queue user handle the event, if they wish
    if(self->event_handler) {
        event_t qevent;
        bzero((char*)&qevent, sizeof(qevent));
        qevent.event_type = et_timer_event;
        qevent.source = self->timer_fd;
        qevent.result = nexpirations;
        qevent.op = eo_read;
        self->event_handler(self, &qevent);
    }
}

void* epoll_handler(void *arg) {
    int res;
    event_queue_t *self = (event_queue_t*)arg;
    epoll_event events[MAX_IO_EVENT_PROCESSING_BATCH_SIZE];
    
    do {
        res = epoll_wait(self->epoll_fd, events, MAX_IO_EVENT_PROCESSING_BATCH_SIZE, -1);
        // epoll_wait might return with EINTR in some cases (in
        // particular under GDB), we just need to retry.
        if(res == -1 && errno == EINTR) {
            if(self->dying)
                break;
            else
                continue;
        }
        check("Waiting for epoll events failed", res == -1);

        for(int i = 0; i < res; i++) {
            if(events[i].data.fd == self->aio_notify_fd) {
                process_aio_notify(self);
                continue;
            }
            if(events[i].data.fd == self->timer_fd) {
                process_timer_notify(self);
                continue;
            }
            if(events[i].events == EPOLLIN ||
               events[i].events == EPOLLOUT)
            {
                if(self->event_handler) {
                    event_t qevent;
                    bzero((char*)&qevent, sizeof(qevent));
                    qevent.event_type = et_sock_event;
                    qevent.source = events[i].data.fd;
                    qevent.state = events[i].data.ptr;
                    if(events[i].events & EPOLLIN)
                        qevent.op = eo_read;
                    else
                        qevent.op = eo_write;
                    self->event_handler(self, &qevent);
                }
            }
            if(events[i].events == EPOLLRDHUP ||
               events[i].events == EPOLLERR ||
               events[i].events == EPOLLHUP) {
                queue_forget_resource(self, events[i].data.fd);
                close(events[i].data.fd);
            }
        }
    } while(1);
    return NULL;
}

void create_event_queue(event_queue_t *event_queue, int queue_id, event_handler_t event_handler,
                        worker_pool_t *parent_pool) {
    int res;
    event_queue->queue_id = queue_id;
    event_queue->event_handler = event_handler;
    event_queue->parent_pool = parent_pool;
    event_queue->dying = false;
    event_queue->timer_fd = -1;
    event_queue->total_expirations = 0;

    // Initialize the allocator using placement new
    new ((void*)&event_queue->alloc) event_queue_t::small_obj_alloc_t();
    
    // Create aio context
    event_queue->aio_context = 0;
    res = io_setup(MAX_CONCURRENT_IO_REQUESTS, &event_queue->aio_context);
    check("Could not setup aio context", res != 0);
    
    // Create a poll fd
    event_queue->epoll_fd = epoll_create(CONCURRENT_NETWORK_EVENTS_COUNT_HINT);
    check("Could not create epoll fd", event_queue->epoll_fd == -1);

    // Start the epoll thread
    res = pthread_create(&event_queue->epoll_thread, NULL, epoll_handler, (void*)event_queue);
    check("Could not create epoll thread", res != 0);

    // Create aio notify fd
    event_queue->aio_notify_fd = eventfd(0, 0);
    check("Could not create aio notification fd", event_queue->aio_notify_fd == -1);

    res = fcntl(event_queue->aio_notify_fd, F_SETFL, O_NONBLOCK);
    check("Could not make aio notify socket non-blocking", res != 0);

    queue_watch_resource(event_queue, event_queue->aio_notify_fd, eo_read, NULL);
    
    // Set thread affinity
    int ncpus = get_cpu_count();
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(queue_id % ncpus, &mask);
    res = pthread_setaffinity_np(event_queue->epoll_thread, sizeof(cpu_set_t), &mask);
    check("Could not set thread affinity", res != 0);
}

void destroy_event_queue(event_queue_t *event_queue) {
    int res;

    event_queue->dying = true;

    // Stop the timer
    queue_stop_timer(event_queue);

    // Kill the threads
    res = pthread_kill(event_queue->epoll_thread, SIGTERM);
    check("Could not send kill signal to epoll thread", res != 0);

    // Wait for the threads to die
    res = pthread_join(event_queue->epoll_thread, NULL);
    check("Could not join with epoll thread", res != 0);
    
    // Cleanup resources
    close(event_queue->aio_notify_fd);
    close(event_queue->epoll_fd);
    io_destroy(event_queue->aio_context);
    (&event_queue->alloc)->~objectheap_alloc_t();
}

void queue_watch_resource(event_queue_t *event_queue, resource_t resource,
                          event_op_t watch_mode, void *state) {
    epoll_event event;
    
    // only trigger if new events come in
    event.events = EPOLLET;
    if(watch_mode == eo_read)
        event.events |= EPOLLIN;
    else
        event.events |= EPOLLOUT;
    event.data.ptr = state;
    
    event.data.fd = resource;
    int res = epoll_ctl(event_queue->epoll_fd, EPOLL_CTL_ADD, resource, &event);
    check("Could not pass socket to worker", res != 0);
}

void queue_forget_resource(event_queue_t *event_queue, resource_t resource) {
    epoll_event event;
    event.events = EPOLLIN;
    event.data.ptr = NULL;
    event.data.fd = resource;
    int res = epoll_ctl(event_queue->epoll_fd, EPOLL_CTL_DEL, resource, &event);
    check("Could remove socket from watching", res != 0);
}

void queue_init_timer(event_queue_t *event_queue, time_t secs) {
    int res = -1;

    // Kill the old timer first (if necessary)
    if(event_queue->timer_fd != -1) {
        queue_stop_timer(event_queue);
    }

    // Create the timer
    event_queue->timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    check("Could not create timer", event_queue->timer_fd < 0);

    // Set the timer_fd to be nonblocking
    res = fcntl(event_queue->timer_fd, F_SETFL, O_NONBLOCK);
    check("Could not make timer non-blocking", res != 0);

    // Arm the timer
    itimerspec timer_spec;
    bzero(&timer_spec, sizeof(timer_spec));
    
    timer_spec.it_value.tv_sec = secs;
    timer_spec.it_value.tv_nsec = 0;
    timer_spec.it_interval.tv_sec = secs;
    timer_spec.it_interval.tv_nsec = 0;
    
    res = timerfd_settime(event_queue->timer_fd, 0, &timer_spec, NULL);
    check("Could not arm the timer.", res != 0);

    // Watch the timer
    queue_watch_resource(event_queue, event_queue->timer_fd, eo_read, NULL);
}

void queue_stop_timer(event_queue_t *event_queue) {
    if(event_queue->timer_fd == -1)
        return;
    
    // Stop watching the timer
    queue_forget_resource(event_queue, event_queue->timer_fd);
    
    int res = -1;
    // Disarm the timer (should happen automatically on close, but what the hell)
    itimerspec timer_spec;
    bzero(&timer_spec, sizeof(timer_spec));
    res = timerfd_settime(event_queue->timer_fd, 0, &timer_spec, NULL);
    check("Could not disarm the timer.", res != 0);
    
    // Close the timer fd
    res = close(event_queue->timer_fd);
    check("Could not close the timer.", res != 0);
    event_queue->timer_fd = -1;
}

