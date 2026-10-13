#include "netcore/loop.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>

uint64_t nc_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

void nc_loop_init(nc_loop_t *loop)
{
    memset(loop, 0, sizeof(*loop));
}

int nc_loop_add(nc_loop_t *loop, const nc_source_t *src)
{
    if (loop->count >= NC_LOOP_MAX_SOURCES)
        return -1;
    loop->src[loop->count++] = *src;
    return 0;
}

nc_source_t nc_source_from_tunnel(const tunnel_t *t)
{
    nc_source_t s;

    s.get_fd = t->ops->get_fd;
    s.wants_write = t->ops->wants_write;
    s.on_readable = t->ops->on_readable;
    s.on_writable = t->ops->on_writable;
    s.next_timeout_ms = t->ops->next_timeout_ms;
    s.on_timer = t->ops->on_timer;
    s.ctx = t->ctx;
    return s;
}

int nc_loop_run_once(nc_loop_t *loop, int max_wait_ms)
{
    struct pollfd pfd[NC_LOOP_MAX_SOURCES];
    int idx[NC_LOOP_MAX_SOURCES];
    int nfds = 0, timeout = max_wait_ms, i, rc;

    for (i = 0; i < loop->count; i++) {
        nc_source_t *s = &loop->src[i];
        int fd = s->get_fd ? s->get_fd(s->ctx) : -1;

        if (s->next_timeout_ms) {
            int t = s->next_timeout_ms(s->ctx);
            if (t >= 0 && (timeout < 0 || t < timeout))
                timeout = t;
        }
        if (fd < 0)
            continue;
        pfd[nfds].fd = fd;
        pfd[nfds].events = POLLIN;
        if (s->wants_write && s->wants_write(s->ctx))
            pfd[nfds].events |= POLLOUT;
        pfd[nfds].revents = 0;
        idx[nfds++] = i;
    }

    rc = poll(pfd, (nfds_t)nfds, timeout);
    if (rc < 0 && errno == EINTR)
        rc = 0;
    if (rc < 0)
        return rc;

    for (i = 0; i < nfds; i++) {
        nc_source_t *s = &loop->src[idx[i]];
        short re = pfd[i].revents;

        /* The fd may have changed inside an earlier handler of the same source. */
        if (!re || !s->get_fd || s->get_fd(s->ctx) != pfd[i].fd)
            continue;
        if ((re & (POLLOUT | POLLERR | POLLHUP)) && s->on_writable &&
            s->wants_write && s->wants_write(s->ctx))
            s->on_writable(s->ctx);
        if (s->get_fd(s->ctx) != pfd[i].fd)
            continue;
        if ((re & (POLLIN | POLLERR | POLLHUP)) && s->on_readable)
            s->on_readable(s->ctx);
    }

    for (i = 0; i < loop->count; i++) {
        nc_source_t *s = &loop->src[i];
        if (s->next_timeout_ms && s->on_timer && s->next_timeout_ms(s->ctx) == 0)
            s->on_timer(s->ctx);
    }
    return rc;
}

void nc_loop_run(nc_loop_t *loop, uint64_t deadline_ms)
{
    while (!loop->stop) {
        int wait = -1;

        if (deadline_ms) {
            uint64_t now = nc_now_ms();
            if (now >= deadline_ms)
                break;
            wait = (int)(deadline_ms - now);
        }
        if (nc_loop_run_once(loop, wait) < 0)
            break;
    }
}
