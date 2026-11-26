/*
 * Two-player co-op over TCP
 *
 * The host (--host) runs the game; the joiner (--join) sends keys and actions and draws the state it receives.
 * Each terminal draws its own view. The joiner recomputes the beams itself from the received object state.
 *
 * Message: [type 1 byte][length 2 bytes][body]  (network byte order, floats sent as their bits)
 *   joiner -> host    HELLO  magic 'PRSM', version
 *                     INPUT  held keys, view mode, walking reference (every tick); with mouse look, also the view (yaw, pitch)
 *                     ACTION take/place, turn, reset, undo, hint, jump
 *   host -> joiner    WELCOME player number
 *                     LEVEL  the room map (on joining and when the room changes)
 *                     STATE  players, object positions and angles, puzzle state, messages for that player (every tick)
 *                     BYE    why the connection was refused or ended
 *
 * Sockets are non-blocking; main's poll loop calls net_handle when they are readable.
 */
#ifndef PRISM_NET_H
#define PRISM_NET_H

#include <poll.h>
#include <stddef.h>

#define NET_PORT 5462

enum { NET_OFF, NET_HOST, NET_CLIENT };

int net_mode(void);

/* host: wait for a joiner on port. -1 with err on failure */
int net_host(int port, char *err, size_t errlen);

/* join: connect to "address[:port]" and wait for WELCOME and the first state (up to 5 s). -1 with err on failure */
int net_join(const char *addr, char *err, size_t errlen);

/* fill pf with the sockets to poll; returns how many */
int net_pollfds(struct pollfd *pf, int max);

/* handle the sockets poll reported (accept, read messages) */
void net_handle(const struct pollfd *pf, int n);

/* after each tick: the host sends the state, the joiner sends its keys */
void net_after_tick(void);

/* host: the current room was rebuilt from changed level files; resend it */
void net_level_changed(void);

/* joiner: send an action to the host */
void net_send_action(int act);

/* connection status text for the screen */
const char *net_status(void);

/* why a joiner lost the host (NULL if connected) */
const char *net_lost(void);

void net_close(void);

#endif
