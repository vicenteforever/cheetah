/*
* Copyright (c) 2014 Xinjing Chow
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
* 1. Redistributions of source code must retain the above copyright
*    notice, this list of conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright
*    notice, this list of conditions and the following disclaimer in the
*    documentation and/or other materials provided with the distribution.
* 3. The name of the author may not be used to endorse or promote products
*    derived from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
* IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
* OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
* IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
* NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
* DATA, OR PROFITS; OR BUSINESS INTERR
*/
/* kqueue polling policy */
#include <sys/errno.h>
#include <sys/event.h>
#include <assert.h>
#include <memory.h>

#include "cheetah/polling_policy.h"
#include "cheetah/includes.h"
#include "cheetah/log.h"

#define KQUEUE_INIT_EVENT_SIZE 32

struct kqueue_internal{
    int kqueue_fd;
    int nevents;
    int max_events;
    struct kevent * events;
};

/*
* Resize the events to given size.
* Return: -1 on failure, 0 on success.
* @pki: the internal data used by kqueue polling policy.
* @size: the size that we should resize to.
*/
static int kqueue_resize(struct kqueue_internal * pki, int size){
    struct kevent * pke;
    assert(pki != NULL);
    if(pki == NULL){
        LOG("pki is null!!");
        return (-1);
    }

    if((pke = realloc(pki->events, size * sizeof(struct kevent))) == NULL){
        LOG("failed to realloc for events, maybe run out of memory.");
        return (-1);
    }

    pki->events = pke;
    pki->max_events = size;
    return (0);
}

/*
* Create and initialize the internal data used by kqueue polling policy.
* Return value: newly created internal data on success, NULL on failure.
* @r: the reactor which uses this policy.
*/
void * kqueue_init(struct reactor * r){
    struct kqueue_internal * ret;

    assert(r);
    if(r == NULL){
        LOG("r is null!!");
        return NULL;
    }
    
    if((ret = malloc(sizeof(struct kqueue_internal))) == NULL){
        LOG("failed to malloc for kqueue_internal");
        return NULL;
    }

    memset(ret, 0, sizeof(struct kqueue_internal));
    
    if((ret->kqueue_fd = kqueue()) == -1){
        LOG("failed on kqueue(): %s", strerror(errno));
        free(ret);
        return NULL;
    }

    if(kqueue_resize(ret, KQUEUE_INIT_EVENT_SIZE) == -1){
        LOG("failed on kqueue_resize()");
        close(ret->kqueue_fd);
        free(ret);
        return NULL;
    }
    //LOG("allocated %d free events for kqeueu", KQUEUE_INIT_EVENT_SIZE);
    return ret;
}

/* 
* Frees up the internal data used by kqueue polling policy.
* @pki: the internal data.
*/
static void kqueue_free(struct kqueue_internal * pki){
    assert(pki != NULL);
    if(pki == NULL){
        LOG("pki is null!!");
        return;
    }

    if(pki->events){
        free(pki->events);
        pki->events = NULL;
    }
    if(pki->kqueue_fd >= 0){
        close(pki->kqueue_fd);
    }

    free(pki);
}

/*
* Clean up the policy internal data
* @r: the reactor which uses this policy
*/
void kqueue_destroy(struct reactor * r){
    assert(r != NULL);
    if(r == NULL){
        LOG("r is null!!");
        return;
    }
    kqueue_free(r->policy_data);
}

static inline short kqueue_setup_filter(short flags){
    short ret = 0;
    if(flags & E_READ){
        ret = EVFILT_READ;
    }
    if(flags & E_WRITE){
        ret = EVFILT_WRITE;
    }
    return ret;
}


/*
* Register the given file descriptor with this kqueue instance.
* Return: 0 on success, -1 on failure.
* @r: the reactor which uses this policy.
* @fd: the file descriptor to listen.
* @flags: the interested events.
*/
int kqueue_add(struct reactor * r, el_socket_t fd, short flags){
    struct kqueue_internal * pki;
    struct kevent e;
    int ret;
    uintptr_t ident;
    short filter;
    ushort action;

    assert(r != NULL);
    if(r == NULL){
        LOG("r is null!!");
        return (-1);
    } else if(flags & E_EDGE) {
        LOG("kqueue does not support edge-triggered mode.")
    }

    pki = r->policy_data;
    if(pki == NULL){
        LOG("pki is null!!");
        return (-1);
    }

    if(pki->nevents >= pki->max_events){
        LOG("resize to %d", pki->max_events << 1);
        if(kqueue_resize(pki, pki->max_events << 1) == -1){
            LOG("failed on kqueue_resize");
            return (-1);
        }
    }
    ident = fd;
    filter = kqueue_setup_filter(flags);
    action = EV_ADD;
    EV_SET(&e, ident, filter, action, ((flags & E_ONCE) ? EV_ONESHOT : 0), 0, NULL);

    //LOG("Registering kqueue event: fd %d filter %d action %d flags %d", ident, filter, action, ((flags & E_ONCE) ? EV_ONESHOT : 0));
    ret = kevent(pki->kqueue_fd, &e, 1, NULL, 0, NULL);

    /* Error handling*/
    if(ret){
        LOG("failed to add event to kqueue: %s", strerror(errno));
        return (-1);
    }
    //LOG("Registered fd %d for envets %d", fd, flags);
    ++pki->nevents;
    return (0);
}

/*
* Unregister the given file descriptor with this kqueue instance.
* Return: -1 on failure, 0 on success.
* @r: the reactor which uses this policy.
* @fd: the file descriptor to remove.
* @flags: the interested events.
*/
int kqueue_del(struct reactor * r, el_socket_t fd, short flags){
    struct kqueue_internal * pki;
    struct kevent e;
    int ret;
    uintptr_t ident;
    short filter;
    ushort action;

    assert(r != NULL);
    if(r == NULL){
        LOG("r is null!!");
        return (-1);
    }

    pki = r->policy_data;
    if(pki == NULL){
        LOG("pki is null!!");
        return (-1);
    }

    ident = fd;
    filter = kqueue_setup_filter(flags);
    action = EV_DELETE;
    EV_SET(&e, ident, filter, action, 0, 0, NULL);
    
    ret = kevent(pki->kqueue_fd, &e, 1, NULL, 0, NULL);

    if(ret){
        LOG("failed to delete event from kqueue: %s", strerror(errno));
        return (-1);
    }

    --pki->nevents;
    return (0);
}

/*
* Polling the file descriptor via kqueue and add active events to the pending_list of the reactor.
* @r: the reactor which uses this policy.
* @timeout: the time after which the select will return.
*/
int kqueue_poll(struct reactor * r, struct timeval * timeout){
    int res_flags , nreadys, i;
    struct kqueue_internal * pki;
    struct event * e;
    struct timespec t;

    assert(r != NULL);

    pki = r->policy_data;
    
    assert(pki != NULL);

    if (timeout) {
        t.tv_sec = timeout->tv_sec;
        t.tv_nsec = timeout->tv_usec * 1000;
    }
    el_lock_unlock(r->lock);
    nreadys = kevent(pki->kqueue_fd, NULL, 0,
                     pki->events, pki->nevents,
                     timeout ? &t : NULL);
    el_lock_lock(r->lock);
    //LOG("kevent: %d events are ready", nreadys);
    for(i = 0; i < nreadys; ++i){
        res_flags = 0;
        if(pki->events[i].filter == EVFILT_READ){
            res_flags = E_READ;
        }
        if(pki->events[i].filter == EVFILT_WRITE){
            res_flags = E_WRITE;
        }
        if(pki->events[i].flags == EV_ERROR){
            LOG("kevent's EV_ERROR flag is set: %s", strerror(errno));
        }
        if(res_flags){
            e = event_ht_retrieve(&r->eht, pki->events[i].ident);
                
            assert(e != NULL);
            if(e == NULL){
                LOG("the event with [fd %d] is not in the hashtable", pki->events[i].ident);
            }else{
                reactor_add_to_pending(r, e, res_flags);
            }
        }
    }

    return nreadys;
}
/* Dumps out the internal data of kqueue policy for debugging. */
void kqueue_print(struct reactor * r){
    LOG("empty implementation of kqueue_print.");
}
