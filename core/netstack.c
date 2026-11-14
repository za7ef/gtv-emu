/* gtv-emu — شبكة المستخدم (NAT) على غرار slirp:
 * الضيف 10.0.2.15، والبوابة 10.0.2.2 (تمثّل المضيف 127.0.0.1)، وخادم الأسماء 10.0.2.3.
 * يدعم ARP وDHCP وping إلى البوابة وUDP (ومنه DNS) وTCP عبر مقابس المضيف.
 */
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static const uint8_t GW_MAC[6] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };
static uint8_t guest_mac[6];
#define IP(a,b,c,d) (((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))
#define GUEST_IP IP(10,0,2,15)
#define GW_IP    IP(10,0,2,2)
#define DNS_IP   IP(10,0,2,3)

static void (*emit)(void *ctx, const uint8_t *f, uint32_t n);
static void *emit_ctx;

static uint32_t rd32(const uint8_t *p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint16_t rd16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }

static uint32_t csum_add(uint32_t s, const uint8_t *p, int n) {
    for (int i = 0; i + 1 < n; i += 2) s += (p[i] << 8) | p[i + 1];
    if (n & 1) s += p[n - 1] << 8;
    return s;
}
static uint16_t csum_fin(uint32_t s) { while (s >> 16) s = (s & 0xFFFF) + (s >> 16); return ~s; }

/* يبني إطار IPv4 من المصدر src إلى الضيف ويرسله */
static void send_ip(uint32_t src, uint8_t proto, const uint8_t *l4, int l4len) {
    uint8_t f[1600];
    if (l4len > 1500 - 20) return;
    memcpy(f, guest_mac, 6); memcpy(f + 6, GW_MAC, 6); wr16(f + 12, 0x0800);
    uint8_t *ip = f + 14;
    static uint16_t ipid;
    ip[0] = 0x45; ip[1] = 0; wr16(ip + 2, 20 + l4len); wr16(ip + 4, ipid++); wr16(ip + 6, 0x4000);
    ip[8] = 64; ip[9] = proto; ip[10] = ip[11] = 0;
    wr32(ip + 12, src); wr32(ip + 16, GUEST_IP);
    wr16(ip + 10, csum_fin(csum_add(0, ip, 20)));
    memcpy(ip + 20, l4, l4len);
    if (proto == 6 || proto == 17) {
        uint8_t ph[12]; wr32(ph, src); wr32(ph + 4, GUEST_IP); ph[8] = 0; ph[9] = proto; wr16(ph + 10, l4len);
        uint8_t *l = ip + 20;
        int co = proto == 6 ? 16 : 6;
        l[co] = l[co + 1] = 0;
        uint16_t c = csum_fin(csum_add(csum_add(0, ph, 12), l, l4len));
        if (proto == 17 && c == 0) c = 0xFFFF;
        wr16(l + co, c);
    }
    emit(emit_ctx, f, 14 + 20 + l4len);
}

static void send_udp(uint32_t src, uint16_t sport, uint16_t dport, const uint8_t *d, int n) {
    uint8_t u[1500];
    if (n > 1472) n = 1472;
    wr16(u, sport); wr16(u + 2, dport); wr16(u + 4, 8 + n); u[6] = u[7] = 0;
    memcpy(u + 8, d, n);
    send_ip(src, 17, u, 8 + n);
}

/* تحويل عنوان الضيف الخارجي إلى عنوان على المضيف */
static int host_addr(uint32_t dst, uint16_t port, struct sockaddr_storage *sa, socklen_t *len);

/* ---------------- DHCP ---------------- */
static void dhcp(const uint8_t *d, int n) {
    if (n < 240 || d[0] != 1) return;
    int type = 0;
    for (int i = 240; i + 1 < n && d[i] != 255; ) {
        if (d[i] == 0) { i++; continue; }
        if (d[i] == 53 && d[i + 1] >= 1) type = d[i + 2];
        i += 2 + d[i + 1];
    }
    if (type != 1 && type != 3) return; /* DISCOVER أو REQUEST */
    uint8_t r[300]; memset(r, 0, sizeof r);
    r[0] = 2; r[1] = 1; r[2] = 6;
    memcpy(r + 4, d + 4, 4);          /* xid */
    wr32(r + 16, GUEST_IP);           /* yiaddr */
    wr32(r + 20, GW_IP);              /* siaddr */
    memcpy(r + 28, d + 28, 16);       /* chaddr */
    wr32(r + 236, 0x63825363);
    uint8_t *o = r + 240;
    *o++ = 53; *o++ = 1; *o++ = type == 1 ? 2 : 5;  /* OFFER / ACK */
    *o++ = 54; *o++ = 4; wr32(o, GW_IP); o += 4;
    *o++ = 51; *o++ = 4; wr32(o, 86400); o += 4;
    *o++ = 1;  *o++ = 4; wr32(o, IP(255,255,255,0)); o += 4;
    *o++ = 3;  *o++ = 4; wr32(o, GW_IP); o += 4;
    *o++ = 6;  *o++ = 4; wr32(o, DNS_IP); o += 4;
    *o++ = 255;
    send_udp(GW_IP, 67, 68, r, o - r);
}

/* ---------------- UDP ---------------- */
#define NUDP 64
typedef struct { int fd; uint32_t dst; uint16_t sport, dport; uint64_t last; } UdpConn;
static UdpConn udp[NUDP];

static char dns_server[64];

static void udp_out(uint32_t dst, uint16_t sport, uint16_t dport, const uint8_t *d, int n) {
    if (dport == 67) { dhcp(d, n); return; }
    UdpConn *c = NULL;
    for (int i = 0; i < NUDP; i++)
        if (udp[i].fd > 0 && udp[i].dst == dst && udp[i].sport == sport && udp[i].dport == dport) { c = &udp[i]; break; }
    if (!c) {
        int oldest = 0;
        for (int i = 0; i < NUDP; i++) {
            if (udp[i].fd <= 0) { oldest = i; break; }
            if (udp[i].last < udp[oldest].last) oldest = i;
        }
        c = &udp[oldest];
        if (c->fd > 0) close(c->fd);
        struct sockaddr_storage sa; socklen_t sl;
        if (host_addr(dst, dport, &sa, &sl) < 0) { c->fd = 0; return; }
        c->fd = socket(sa.ss_family, SOCK_DGRAM, 0);
        if (c->fd < 0) { c->fd = 0; return; }
        fcntl(c->fd, F_SETFL, O_NONBLOCK);
        if (connect(c->fd, (struct sockaddr *)&sa, sl) < 0) { close(c->fd); c->fd = 0; return; }
        c->dst = dst; c->sport = sport; c->dport = dport;
    }
    c->last = host_ns();
    if (send(c->fd, d, n, 0) < 0) {}
}

static void udp_poll(void) {
    uint8_t b[1472];
    for (int i = 0; i < NUDP; i++) {
        if (udp[i].fd <= 0) continue;
        ssize_t r;
        while ((r = recv(udp[i].fd, b, sizeof b, 0)) > 0)
            send_udp(udp[i].dst, udp[i].dport, udp[i].sport, b, r);
    }
}

/* ---------------- TCP ---------------- */
#define NTCP 64
#define SBUF (256 * 1024)
enum { T_FREE, T_CONNECTING, T_EST, T_CLOSED };
typedef struct {
    int st, fd;
    uint32_t dst; uint16_t sport, dport;
    uint32_t rcv_nxt;           /* ما ننتظره من الضيف */
    uint32_t snd_una, snd_nxt;  /* أرقامنا نحو الضيف */
    uint32_t iss;
    uint32_t wnd;               /* نافذة الضيف */
    uint8_t *sbuf; uint32_t slen; /* بيانات من المضيف لم يؤكدها الضيف بعد، تبدأ عند snd_una */
    int host_eof, fin_sent, guest_fin;
    uint64_t rto;
} TcpConn;
static TcpConn tcp[NTCP];

static void tcp_send(TcpConn *c, uint8_t flags, uint32_t seq, const uint8_t *d, int n) {
    uint8_t t[1500];
    int hl = (flags & 0x02) ? 24 : 20;
    wr16(t, c->dport); wr16(t + 2, c->sport);
    wr32(t + 4, seq); wr32(t + 8, (flags & 0x10) ? c->rcv_nxt : 0);
    t[12] = (hl / 4) << 4; t[13] = flags;
    wr16(t + 14, 65535); t[16] = t[17] = 0; t[18] = t[19] = 0;
    if (hl == 24) { t[20] = 2; t[21] = 4; wr16(t + 22, 1460); } /* MSS */
    if (n) memcpy(t + hl, d, n);
    send_ip(c->dst, 6, t, hl + n);
}

static void tcp_rst_reply(uint32_t dst, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack) {
    TcpConn tmp = { .dst = dst, .sport = sport, .dport = dport, .rcv_nxt = ack };
    tcp_send(&tmp, 0x14, seq, NULL, 0);
}

static void tcp_free(TcpConn *c) {
    if (c->fd > 0) close(c->fd);
    free(c->sbuf);
    memset(c, 0, sizeof *c);
}

static void tcp_out(uint32_t dst, const uint8_t *t, int n) {
    if (n < 20) return;
    uint16_t sport = rd16(t), dport = rd16(t + 2);
    uint32_t seq = rd32(t + 4), ack = rd32(t + 8);
    int hl = (t[12] >> 4) * 4; uint8_t fl = t[13];
    uint16_t wnd = rd16(t + 14);
    const uint8_t *d = t + hl; int dl = n - hl;
    if (hl < 20 || dl < 0) return;
    TcpConn *c = NULL;
    for (int i = 0; i < NTCP; i++)
        if (tcp[i].st != T_FREE && tcp[i].dst == dst && tcp[i].sport == sport && tcp[i].dport == dport) { c = &tcp[i]; break; }
    if (fl & 0x04) { if (c) tcp_free(c); return; } /* RST */
    if (fl & 0x02) { /* SYN: اتصال جديد */
        if (c) return; /* تكرار SYN أثناء الاتصال */
        for (int i = 0; i < NTCP; i++) if (tcp[i].st == T_FREE) { c = &tcp[i]; break; }
        struct sockaddr_storage sa; socklen_t sl;
        if (!c || host_addr(dst, dport, &sa, &sl) < 0) { tcp_rst_reply(dst, sport, dport, 0, seq + 1); return; }
        c->fd = socket(sa.ss_family, SOCK_STREAM, 0);
        if (c->fd < 0) { c->fd = 0; tcp_rst_reply(dst, sport, dport, 0, seq + 1); return; }
        fcntl(c->fd, F_SETFL, O_NONBLOCK);
        int one = 1; setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (connect(c->fd, (struct sockaddr *)&sa, sl) < 0 && errno != EINPROGRESS) {
            close(c->fd); c->fd = 0; tcp_rst_reply(dst, sport, dport, 0, seq + 1); return;
        }
        c->st = T_CONNECTING; c->dst = dst; c->sport = sport; c->dport = dport;
        c->rcv_nxt = seq + 1;
        c->iss = (uint32_t)host_ns() * 2654435761u;
        c->snd_una = c->snd_nxt = c->iss;
        c->wnd = wnd;
        c->sbuf = malloc(SBUF);
        return;
    }
    if (!c) { if (!(fl & 0x04)) tcp_rst_reply(dst, sport, dport, ack, seq + dl); return; }
    c->wnd = wnd;
    if (fl & 0x10) { /* ACK */
        uint32_t acked = ack - c->snd_una;
        uint32_t inflight = c->snd_nxt - c->snd_una;
        if (acked && acked <= inflight) {
            uint32_t data_acked = acked;
            if (c->snd_una == c->iss) data_acked--;                      /* SYN */
            if (c->fin_sent && ack == c->snd_nxt) data_acked--;          /* FIN */
            if (data_acked > c->slen) data_acked = c->slen;
            memmove(c->sbuf, c->sbuf + data_acked, c->slen - data_acked);
            c->slen -= data_acked;
            c->snd_una = ack;
            c->rto = host_ns() + 1000000000ULL;
            if (c->st == T_CONNECTING) c->st = T_EST;
        }
    }
    if (dl > 0) {
        if (seq == c->rcv_nxt && c->fd > 0) {
            ssize_t w = send(c->fd, d, dl, MSG_NOSIGNAL);
            if (w > 0) c->rcv_nxt += w;
        }
        tcp_send(c, 0x10, c->snd_nxt, NULL, 0);
    }
    if ((fl & 0x01) && seq + dl == c->rcv_nxt && !c->guest_fin) { /* FIN من الضيف */
        c->guest_fin = 1;
        c->rcv_nxt++;
        if (c->fd > 0) shutdown(c->fd, SHUT_WR);
        tcp_send(c, 0x10, c->snd_nxt, NULL, 0);
    }
    if (c->guest_fin && c->fin_sent && c->snd_una == c->snd_nxt) tcp_free(c);
}

static void tcp_poll(void) {
    uint64_t now = host_ns();
    for (int i = 0; i < NTCP; i++) {
        TcpConn *c = &tcp[i];
        if (c->st == T_FREE) continue;
        if (c->st == T_CONNECTING && c->snd_nxt == c->iss) {
            struct pollfd p = { c->fd, POLLOUT, 0 };
            if (poll(&p, 1, 0) <= 0) continue;
            int err = 0; socklen_t el = sizeof err;
            getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el);
            if (err) { tcp_rst_reply(c->dst, c->sport, c->dport, 0, c->rcv_nxt); tcp_free(c); continue; }
            tcp_send(c, 0x12, c->iss, NULL, 0); /* SYN-ACK */
            c->snd_nxt = c->iss + 1;
            c->rto = now + 1000000000ULL;
            continue;
        }
        if (c->st == T_CONNECTING) {
            if (now > c->rto) { tcp_send(c, 0x12, c->iss, NULL, 0); c->rto = now + 1000000000ULL; }
            continue;
        }
        /* قراءة بيانات جديدة من المضيف */
        if (!c->host_eof && c->slen < SBUF) {
            ssize_t r = recv(c->fd, c->sbuf + c->slen, SBUF - c->slen, 0);
            if (r > 0) c->slen += r;
            else if (r == 0) c->host_eof = 1;
            else if (errno != EAGAIN && errno != EWOULDBLOCK) c->host_eof = 1;
        }
        /* إعادة الإرسال عند انتهاء المهلة */
        if (c->snd_nxt != c->snd_una && now > c->rto) {
            c->snd_nxt = c->snd_una;
            if (c->fin_sent) c->fin_sent = 0;
        }
        /* إرسال ما تسمح به نافذة الضيف */
        for (;;) {
            uint32_t off = c->snd_nxt - c->snd_una;
            uint32_t inflight = off;
            if (off >= c->slen) break;
            if (inflight >= c->wnd) break;
            uint32_t n = c->slen - off;
            if (n > 1460) n = 1460;
            if (n > c->wnd - inflight) n = c->wnd - inflight;
            tcp_send(c, 0x18, c->snd_nxt, c->sbuf + off, n);
            c->snd_nxt += n;
            c->rto = now + 1000000000ULL;
        }
        if (c->host_eof && !c->fin_sent && c->snd_nxt - c->snd_una == c->slen) {
            tcp_send(c, 0x11, c->snd_nxt, NULL, 0);
            c->snd_nxt++; c->fin_sent = 1;
            c->rto = now + 1000000000ULL;
        }
        if (c->guest_fin && c->fin_sent && c->snd_una == c->snd_nxt) tcp_free(c);
    }
}

/* ---------------- العناوين ---------------- */
static int host_addr(uint32_t dst, uint16_t port, struct sockaddr_storage *sa, socklen_t *len) {
    memset(sa, 0, sizeof *sa);
    if (dst == DNS_IP && port == 53 && dns_server[0]) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)sa;
        struct sockaddr_in *s4 = (struct sockaddr_in *)sa;
        if (inet_pton(AF_INET6, dns_server, &s6->sin6_addr) == 1) {
            s6->sin6_family = AF_INET6; s6->sin6_port = htons(53); *len = sizeof *s6; return 0;
        }
        if (inet_pton(AF_INET, dns_server, &s4->sin_addr) == 1) {
            s4->sin_family = AF_INET; s4->sin_port = htons(53); *len = sizeof *s4; return 0;
        }
        return -1;
    }
    struct sockaddr_in *s = (struct sockaddr_in *)sa;
    s->sin_family = AF_INET; s->sin_port = htons(port);
    if (dst == GW_IP) s->sin_addr.s_addr = htonl(IP(127,0,0,1)); /* البوابة = المضيف نفسه */
    else if ((dst >> 8) == (IP(10,0,2,0) >> 8)) return -1;
    else s->sin_addr.s_addr = htonl(dst);
    *len = sizeof *s;
    return 0;
}

/* ---------------- نقطة الدخول ---------------- */
void netstack_init(void (*fn)(void *, const uint8_t *, uint32_t), void *ctx, const uint8_t mac[6]) {
    emit = fn; emit_ctx = ctx;
    memcpy(guest_mac, mac, 6);
    FILE *f = fopen("/etc/resolv.conf", "r");
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        char a[64];
        if (sscanf(line, "nameserver %63s", a) == 1) { snprintf(dns_server, sizeof dns_server, "%s", a); break; }
    }
    if (f) fclose(f);
    if (!dns_server[0]) snprintf(dns_server, sizeof dns_server, "8.8.8.8");
}

void netstack_tx(const uint8_t *f, uint32_t n) {
    if (n < 14) return;
    uint16_t et = rd16(f + 12);
    if (et == 0x0806 && n >= 42 && rd16(f + 20) == 1) { /* طلب ARP */
        uint32_t tip = rd32(f + 38);
        if ((tip >> 8) != (IP(10,0,2,0) >> 8) || tip == GUEST_IP) return;
        uint8_t o[42];
        memcpy(o, f + 6, 6); memcpy(o + 6, GW_MAC, 6); wr16(o + 12, 0x0806);
        memcpy(o + 14, f + 14, 6); wr16(o + 20, 2);
        memcpy(o + 22, GW_MAC, 6); wr32(o + 28, tip);
        memcpy(o + 32, f + 22, 6); memcpy(o + 38, f + 28, 4);
        emit(emit_ctx, o, 42);
        return;
    }
    if (et != 0x0800 || n < 34) return;
    const uint8_t *ip = f + 14;
    int ihl = (ip[0] & 15) * 4, tot = rd16(ip + 2);
    if (tot + 14 > (int)n || tot < ihl) return;
    uint32_t dst = rd32(ip + 16);
    const uint8_t *l4 = ip + ihl; int l4n = tot - ihl;
    switch (ip[9]) {
    case 1: /* ICMP: ping إلى البوابة فقط */
        if (dst == GW_IP && l4n >= 8 && l4[0] == 8) {
            uint8_t r[1500]; memcpy(r, l4, l4n);
            r[0] = 0; r[2] = r[3] = 0;
            wr16(r + 2, csum_fin(csum_add(0, r, l4n)));
            send_ip(GW_IP, 1, r, l4n);
        }
        break;
    case 17:
        if (l4n >= 8) udp_out(dst, rd16(l4), rd16(l4 + 2), l4 + 8, rd16(l4 + 4) - 8 <= l4n - 8 ? rd16(l4 + 4) - 8 : l4n - 8);
        break;
    case 6:
        tcp_out(dst, l4, l4n);
        break;
    }
}

void netstack_poll(void) {
    udp_poll();
    tcp_poll();
}

int netstack_active(void) {
    for (int i = 0; i < NTCP; i++) if (tcp[i].st != T_FREE) return 1;
    return 0;
}
