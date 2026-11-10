/*
 * 2인 협동 네트워크 (TCP)
 *
 * 호스트(--host)가 게임을 실제로 진행하고, 참가자(--join)는 키와 조작을 보내고 상태를 받아 그린다.
 * 화면은 각자 자기 터미널에서 자기 시점으로 그린다. 빛줄기는 받은 물체 상태로 참가자가 직접 다시 계산한다.
 *
 * 메시지: [종류 1바이트][길이 2바이트][내용]  (네트워크 바이트 순서, 실수는 비트 그대로)
 *   참가자 → 호스트   HELLO  매직 'PRSM', 버전
 *                     INPUT  누르는 키, 시점, 걷는 방향 기준 (매 틱)
 *                     ACTION 들기·놓기, 돌리기, 처음 상태로
 *   호스트 → 참가자   WELCOME 플레이어 번호
 *                     LEVEL  방 지도 (들어올 때, 방이 바뀔 때)
 *                     STATE  플레이어들, 물체 위치·각도, 퍼즐 상태, 그 사람에게 보낼 알림 (매 틱)
 *                     BYE    거절이나 끝내는 이유
 *
 * 소켓은 논블로킹으로 두고 main의 poll 루프가 읽을 수 있을 때 net_handle을 부른다.
 */
#ifndef PRISM_NET_H
#define PRISM_NET_H

#include <poll.h>
#include <stddef.h>

#define NET_PORT 5462

enum { NET_OFF, NET_HOST, NET_CLIENT };

int net_mode(void);

/* 호스트: port에서 참가자를 기다린다. 실패하면 -1과 err */
int net_host(int port, char *err, size_t errlen);

/* 참가: "주소[:포트]"에 접속해 WELCOME과 첫 상태까지 받는다 (최대 5초). 실패하면 -1과 err */
int net_join(const char *addr, char *err, size_t errlen);

/* poll에 넣을 소켓들을 pf에 채우고 개수를 돌려준다 */
int net_pollfds(struct pollfd *pf, int max);

/* poll 결과로 읽을 수 있는 소켓을 처리한다 (접속 받기, 메시지 읽기) */
void net_handle(const struct pollfd *pf, int n);

/* 틱이 끝난 뒤: 호스트는 상태를, 참가자는 키를 보낸다 */
void net_after_tick(void);

/* 참가자: 조작을 호스트로 보낸다 */
void net_send_action(int act);

/* 화면에 보여 줄 연결 상태 글 */
const char *net_status(void);

/* 참가자가 호스트와 끊어졌으면 그 이유 (아니면 NULL) */
const char *net_lost(void);

void net_close(void);

#endif
