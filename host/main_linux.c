/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * main_linux.c — Linux ARM64 (NVIDIA DGX Spark) CLI, Web-Canvas & VNC runner for Windows 11 (`winvm`).
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "winvm.h"
#include "mmu.h"
#include "share.h"

static volatile sig_atomic_t g_sig_stop = 0;

static void on_sig(int sig) {
    (void)sig;
    g_sig_stop = 1;
    hv_winvm_stop();
}

/* Simple ASCII typer using USB HID Keyboard usages */
static void type_ascii_char(char ch) {
    uint8_t usage = 0;
    int shift = 0;
    if (ch >= 'a' && ch <= 'z') { usage = (uint8_t)(0x04 + (ch - 'a')); }
    else if (ch >= 'A' && ch <= 'Z') { usage = (uint8_t)(0x04 + (ch - 'A')); shift = 1; }
    else if (ch >= '1' && ch <= '9') { usage = (uint8_t)(0x1e + (ch - '1')); }
    else if (ch == '0') { usage = 0x27; }
    else {
        switch (ch) {
        case ' ': usage = 0x2c; break;
        case '\n': case '\r': usage = 0x28; break;
        case '\t': usage = 0x2b; break;
        case '-': usage = 0x2d; break;
        case '_': usage = 0x2d; shift = 1; break;
        case '=': usage = 0x2e; break;
        case '+': usage = 0x2e; shift = 1; break;
        case '[': usage = 0x2f; break;
        case '{': usage = 0x2f; shift = 1; break;
        case ']': usage = 0x30; break;
        case '}': usage = 0x30; shift = 1; break;
        case '\\': usage = 0x31; break;
        case '|': usage = 0x31; shift = 1; break;
        case ';': usage = 0x33; break;
        case ':': usage = 0x33; shift = 1; break;
        case '\'': usage = 0x34; break;
        case '"': usage = 0x34; shift = 1; break;
        case '`': usage = 0x35; break;
        case '~': usage = 0x35; shift = 1; break;
        case ',': usage = 0x36; break;
        case '<': usage = 0x36; shift = 1; break;
        case '.': usage = 0x37; break;
        case '>': usage = 0x37; shift = 1; break;
        case '/': usage = 0x38; break;
        case '?': usage = 0x38; shift = 1; break;
        case '!': usage = 0x1e; shift = 1; break;
        case '@': usage = 0x1f; shift = 1; break;
        case '#': usage = 0x20; shift = 1; break;
        case '$': usage = 0x21; shift = 1; break;
        case '%': usage = 0x22; shift = 1; break;
        case '^': usage = 0x23; shift = 1; break;
        case '&': usage = 0x24; shift = 1; break;
        case '*': usage = 0x25; shift = 1; break;
        case '(': usage = 0x26; shift = 1; break;
        case ')': usage = 0x27; shift = 1; break;
        default: return;
        }
    }
    if (shift) { hv_winvm_key_hid(0xe1, 1); usleep(20000); }
    hv_winvm_key_hid(usage, 1);
    usleep(35000);
    hv_winvm_key_hid(usage, 0);
    if (shift) { usleep(20000); hv_winvm_key_hid(0xe1, 0); }
    usleep(35000);
}

static void *cmd_file_thread(void *arg) {
    const char *path = (const char *)arg;
    FILE *f = fopen(path, "a+");
    if (!f) return NULL;
    long pos = 0;
    while (!g_sig_stop) {
        char line[512];
        fseek(f, pos, SEEK_SET);
        if (!fgets(line, sizeof line, f) || !strchr(line, '\n')) {
            clearerr(f);
            usleep(150000);
            continue;
        }
        pos = ftell(f);
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;

        double xn = 0, yn = 0;
        int code = 0;
        if (sscanf(line, "click %lf %lf", &xn, &yn) == 2) {
            hv_winvm_tablet(xn, yn, 0, 0);
            usleep(40000);
            hv_winvm_tablet(xn, yn, 1, 0);
            usleep(80000);
            hv_winvm_tablet(xn, yn, 0, 0);
        } else if (sscanf(line, "rclick %lf %lf", &xn, &yn) == 2) {
            hv_winvm_tablet(xn, yn, 0, 0);
            usleep(40000);
            hv_winvm_tablet(xn, yn, 2, 0);
            usleep(80000);
            hv_winvm_tablet(xn, yn, 0, 0);
        } else if (sscanf(line, "move %lf %lf", &xn, &yn) == 2) {
            hv_winvm_tablet(xn, yn, 0, 0);
        } else if (sscanf(line, "hidkey %i", &code) == 1) {
            hv_winvm_key_hid((uint8_t)code, 1);
            usleep(50000);
            hv_winvm_key_hid((uint8_t)code, 0);
        } else if (!strcmp(line, "win")) {
            hv_winvm_key_hid(0xe3, 1);
            usleep(60000);
            hv_winvm_key_hid(0xe3, 0);
        } else if (!strcmp(line, "winr")) {
            hv_winvm_key_hid(0xe3, 1);
            usleep(30000);
            hv_winvm_key_hid(0x15, 1); /* 'r' */
            usleep(50000);
            hv_winvm_key_hid(0x15, 0);
            usleep(30000);
            hv_winvm_key_hid(0xe3, 0);
        } else if (!strcmp(line, "enter")) {
            hv_winvm_key_hid(0x28, 1);
            usleep(50000);
            hv_winvm_key_hid(0x28, 0);
        } else if (!strcmp(line, "cad")) {
            hv_winvm_send_ctrl_alt_del();
        } else if (!strcmp(line, "power")) {
            hv_winvm_power_button();
        } else if (!strncmp(line, "type ", 5)) {
            for (const char *p = line + 5; *p; p++) type_ascii_char(*p);
        } else if (!strncmp(line, "screenshot ", 11)) {
            int rc = hv_winvm_save_png(line + 11);
            fprintf(stderr, "winvm: screenshot -> %s (rc=%d)\n", line + 11, rc);
        } else if (!strcmp(line, "cpus")) {
            HvMachine *m = hv_winvm_machine();
            for (int i = 0; i < m->ncpus; i++)
                if (m->cpus[i]) __atomic_store_n(&m->cpus[i]->exit_req, 1, __ATOMIC_SEQ_CST);
            usleep(4000);
            for (int i = 0; i < m->ncpus; i++) {
                HvCpu *c = m->cpus[i];
                if (!c) continue;
                uint64_t pc = c->pc;
                fprintf(stderr, "CPU%d: pc=%llx el=%u exit=%u x0=%llx x1=%llx x2=%llx x3=%llx x8=%llx x16=%llx lr=%llx\n",
                        i, (unsigned long long)pc, c->el, c->exit_reason,
                        (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                        (unsigned long long)c->x[2], (unsigned long long)c->x[3],
                        (unsigned long long)c->x[8], (unsigned long long)c->x[16],
                        (unsigned long long)c->x[30]);
                if (pc >= 0x1000) {
                    fprintf(stderr, "  insns:");
                    for (int off = -32; off <= 32; off += 4) {
                        uint64_t va = (pc & ~3ull) + (int64_t)off;
                        HvWalk w = {0};
                        hv_mmu_walk(c, va, HV_ACC_READ, c->el, &w);
                        uint32_t word = 0;
                        if (w.fsc == 0) hv_phys_read(m, w.pa, &word, 4);
                        fprintf(stderr, " %s%08x", off == 0 ? "*" : "", word);
                    }
                    fprintf(stderr, "\n");
                }
            }
            fflush(stderr);
        } else if (sscanf(line, "oauthmode %d", &code) == 1) {
            hv_share_set_url_handler_enabled(code);
            fprintf(stderr, "winvm: oauthmode=%d\n", code);
        } else if (!strcmp(line, "oauthstatus")) {
            char *u = hv_share_take_open_url();
            int done = hv_share_take_oauth_done();
            fprintf(stderr, "OAUTH_STATUS url=%s done=%d\n", u ? u : "(none)", done);
            free(u);
        } else if (sscanf(line, "wait %d", &code) == 1 && code > 0) {
            usleep((useconds_t)code * 1000u);
        } else if (!strcmp(line, "quit")) {
            hv_winvm_stop();
            break;
        }
        fprintf(stderr, "winvm: cmd: %s\n", line);
    }
    fclose(f);
    return NULL;
}

typedef struct TimerArgs {
    const char *screenshot_path;
    int screenshot_after;
    int timeout_sec;
} TimerArgs;

static void *timer_thread(void *arg) {
    TimerArgs *ta = (TimerArgs *)arg;
    int elapsed = 0;
    int shot_taken = 0;
    while (!g_sig_stop) {
        sleep(1);
        elapsed++;
        if (ta->screenshot_path && !shot_taken && ta->screenshot_after > 0 && elapsed >= ta->screenshot_after) {
            int rc = hv_winvm_save_png(ta->screenshot_path);
            uint32_t fw = 0, fh = 0;
            hv_winvm_display_size(&fw, &fh);
            fprintf(stderr, "winvm: saved screenshot %s at %ds (%ux%u, rc=%d)\n",
                    ta->screenshot_path, elapsed, fw, fh, rc);
            shot_taken = 1;
        }
        if (ta->timeout_sec > 0 && elapsed >= ta->timeout_sec) {
            if (ta->screenshot_path && !shot_taken) {
                int rc = hv_winvm_save_png(ta->screenshot_path);
                uint32_t fw = 0, fh = 0;
                hv_winvm_display_size(&fw, &fh);
                fprintf(stderr, "winvm: saved final screenshot %s at %ds (%ux%u, rc=%d)\n",
                        ta->screenshot_path, elapsed, fw, fh, rc);
            }
            fprintf(stderr, "winvm: timeout (%ds) reached — stopping VM.\n", ta->timeout_sec);
            hv_winvm_stop();
            break;
        }
    }
    return NULL;
}

/* ============================================================================
 * Built-in Interactive HTTP Web-Canvas Viewer (--web <port>)
 * ============================================================================ */

static const char kWebIndexHtml[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>WindowsPixel on DGX Spark</title>"
    "<style>"
    "body{margin:0;background:#111;color:#eee;font-family:system-ui,sans-serif;display:flex;flex-direction:column;align-items:center;height:100vh;overflow:hidden}"
    "#bar{display:flex;gap:10px;align-items:center;padding:8px 16px;background:#1e1e24;width:100%;box-sizing:border-box;border-bottom:1px solid #333;font-size:13px}"
    "button{background:#2b2b36;color:#fff;border:1px solid #444;border-radius:6px;padding:5px 10px;cursor:pointer;font-size:12px}"
    "button:hover{background:#3c3c4a}"
    "#status{margin-left:auto;color:#9ca3af;font-family:monospace}"
    "#wrap{flex:1;display:flex;align-items:center;justify-content:center;width:100%;overflow:hidden}"
    "canvas{max-width:100%;max-height:100%;background:#000;box-shadow:0 8px 32px rgba(0,0,0,0.6);cursor:default;outline:none}"
    "</style></head><body>"
    "<div id='bar'>"
    "<strong>Windows 11 ARM64 (DGX Spark)</strong>"
    "<button onclick='sendAct(\"win\")'>Win</button>"
    "<button onclick='sendAct(\"winr\")'>Win+R</button>"
    "<button onclick='sendAct(\"cad\")'>Ctrl+Alt+Del</button>"
    "<button onclick='sendAct(\"power\")'>ACPI Power</button>"
    "<span id='status'>Connecting...</span>"
    "</div>"
    "<div id='wrap'><canvas id='screen' width='1024' height='768' tabindex='0'></canvas></div>"
    "<script>"
    "const cv=document.getElementById('screen'),ctx=cv.getContext('2d'),st=document.getElementById('status');"
    "let btns=0,inFlight=false,fpsCnt=0,lastFps=performance.now(),curFps=0;"
    "async function pollFrame(){"
    "  if(inFlight)return; inFlight=true;"
    "  try{"
    "    const r=await fetch('/frame.rgba',{cache:'no-store'});"
    "    if(r.status===200){"
    "      const w=parseInt(r.headers.get('X-Width')||'0'),h=parseInt(r.headers.get('X-Height')||'0');"
    "      const buf=await r.arrayBuffer();"
    "      if(w>0&&h>0&&buf.byteLength===w*h*4){"
    "        if(cv.width!==w||cv.height!==h){cv.width=w;cv.height=h;}"
    "        const img=new ImageData(new Uint8ClampedArray(buf),w,h);"
    "        ctx.putImageData(img,0,0);"
    "        fpsCnt++; const now=performance.now();"
    "        if(now-lastFps>=1000){curFps=fpsCnt;fpsCnt=0;lastFps=now;}"
    "        st.textContent=`${w}x${h} @ ${curFps} fps`;"
    "      }"
    "    }else{st.textContent='Waiting for UEFI/Windows ramfb...';}"
    "  }catch(e){st.textContent='Disconnected';}"
    "  inFlight=false;"
    "}"
    "setInterval(pollFrame,40);"
    "function normXY(e){"
    "  const r=cv.getBoundingClientRect();"
    "  return [Math.max(0,Math.min(1,(e.clientX-r.left)/r.width)),Math.max(0,Math.min(1,(e.clientY-r.top)/r.height))];"
    "}"
    "function sendMouse(e,w){"
    "  const [x,y]=normXY(e);"
    "  fetch(`/input?t=m&x=${x.toFixed(5)}&y=${y.toFixed(5)}&b=${btns}&w=${w||0}`,{method:'POST'});"
    "}"
    "cv.addEventListener('contextmenu',e=>e.preventDefault());"
    "cv.addEventListener('mousedown',e=>{cv.focus();e.preventDefault();if(e.button===0)btns|=1;if(e.button===1)btns|=4;if(e.button===2)btns|=2;sendMouse(e,0);});"
    "cv.addEventListener('mouseup',e=>{e.preventDefault();if(e.button===0)btns&=~1;if(e.button===1)btns&=~4;if(e.button===2)btns&=~2;sendMouse(e,0);});"
    "cv.addEventListener('mousemove',e=>{sendMouse(e,0);});"
    "cv.addEventListener('wheel',e=>{e.preventDefault();sendMouse(e,e.deltaY<0?1:-1);},{passive:false});"
    "const CODE_MAP={"
    "KeyA:4,KeyB:5,KeyC:6,KeyD:7,KeyE:8,KeyF:9,KeyG:10,KeyH:11,KeyI:12,KeyJ:13,KeyK:14,KeyL:15,KeyM:16,"
    "KeyN:17,KeyO:18,KeyP:19,KeyQ:20,KeyR:21,KeyS:22,KeyT:23,KeyU:24,KeyV:25,KeyW:26,KeyX:27,KeyY:28,KeyZ:29,"
    "Digit1:30,Digit2:31,Digit3:32,Digit4:33,Digit5:34,Digit6:35,Digit7:36,Digit8:37,Digit9:38,Digit0:39,"
    "Enter:40,Escape:41,Backspace:42,Tab:43,Space:44,Minus:45,Equal:46,BracketLeft:47,BracketRight:48,Backslash:49,"
    "Semicolon:51,Quote:52,Backquote:53,Comma:54,Period:55,Slash:56,CapsLock:57,"
    "F1:58,F2:59,F3:60,F4:61,F5:62,F6:63,F7:64,F8:65,F9:66,F10:67,F11:68,F12:69,"
    "Home:74,PageUp:75,Delete:76,End:77,PageDown:78,ArrowRight:79,ArrowLeft:80,ArrowDown:81,ArrowUp:82,"
    "ControlLeft:224,ShiftLeft:225,AltLeft:226,MetaLeft:227,ControlRight:228,ShiftRight:229,AltRight:230,MetaRight:231"
    "};"
    "window.addEventListener('keydown',e=>{const u=CODE_MAP[e.code];if(u){e.preventDefault();fetch(`/input?t=k&u=${u}&d=1`,{method:'POST'});}});"
    "window.addEventListener('keyup',e=>{const u=CODE_MAP[e.code];if(u){e.preventDefault();fetch(`/input?t=k&u=${u}&d=0`,{method:'POST'});}});"
    "function sendAct(a){fetch(`/input?t=a&a=${a}`,{method:'POST'});}"
    "</script></body></html>";

static int send_bytes(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void handle_web_client(int fd) {
    char req[4096];
    ssize_t n = recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0) {
        close(fd);
        return;
    }
    req[n] = 0;

    if (!strncmp(req, "GET / ", 6) || !strncmp(req, "GET /index.html", 15)) {
        char hdr[256];
        size_t body_len = sizeof(kWebIndexHtml) - 1;
        int hl = snprintf(hdr, sizeof hdr,
                          "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                          "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                          body_len);
        send_bytes(fd, hdr, (size_t)hl);
        send_bytes(fd, kWebIndexHtml, body_len);
    } else if (!strncmp(req, "GET /frame.rgba", 15)) {
        HvWinVirt *board = hv_winvm_board();
        const uint8_t *px = NULL;
        uint32_t w = 0, h = 0, st = 0;
        if (!board || !board->fb || hv_fb_display_get_frame(board->fb, &px, &w, &h, &st) != 0) {
            const char *resp = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
            send_bytes(fd, resp, strlen(resp));
        } else {
            size_t total = (size_t)w * h * 4;
            uint8_t *rgba = malloc(total);
            if (rgba) {
                for (uint32_t y = 0; y < h; y++) {
                    const uint8_t *src = px + (size_t)y * st;
                    uint8_t *dst = rgba + (size_t)y * w * 4;
                    for (uint32_t x = 0; x < w; x++) {
                        dst[x * 4 + 0] = src[x * 4 + 2]; /* R */
                        dst[x * 4 + 1] = src[x * 4 + 1]; /* G */
                        dst[x * 4 + 2] = src[x * 4 + 0]; /* B */
                        dst[x * 4 + 3] = 255;            /* A */
                    }
                }
                char hdr[256];
                int hl = snprintf(hdr, sizeof hdr,
                                  "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                                  "X-Width: %u\r\nX-Height: %u\r\n"
                                  "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                                  w, h, total);
                send_bytes(fd, hdr, (size_t)hl);
                send_bytes(fd, rgba, total);
                free(rgba);
            }
        }
    } else if (!strncmp(req, "POST /input?", 12)) {
        const char *q = req + 12;
        double x = 0, y = 0;
        int b = 0, w = 0, u = 0, d = 0;
        if (sscanf(q, "t=m&x=%lf&y=%lf&b=%d&w=%d", &x, &y, &b, &w) >= 3) {
            hv_winvm_tablet(x, y, (uint8_t)b, (int8_t)w);
        } else if (sscanf(q, "t=k&u=%d&d=%d", &u, &d) == 2) {
            hv_winvm_key_hid((uint8_t)u, d);
        } else if (!strncmp(q, "t=a&a=winr", 10)) {
            hv_winvm_key_hid(0xe3, 1); usleep(30000);
            hv_winvm_key_hid(0x15, 1); usleep(50000);
            hv_winvm_key_hid(0x15, 0); usleep(30000);
            hv_winvm_key_hid(0xe3, 0);
        } else if (!strncmp(q, "t=a&a=win", 9)) {
            hv_winvm_key_hid(0xe3, 1); usleep(50000); hv_winvm_key_hid(0xe3, 0);
        } else if (!strncmp(q, "t=a&a=cad", 9)) {
            hv_winvm_send_ctrl_alt_del();
        } else if (!strncmp(q, "t=a&a=power", 11)) {
            hv_winvm_power_button();
        }
        const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        send_bytes(fd, resp, strlen(resp));
    } else {
        const char *resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_bytes(fd, resp, strlen(resp));
    }
    close(fd);
}

static void *web_server_thread(void *arg) {
    int port = (int)(intptr_t)arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return NULL;
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(s, 32) != 0) {
        fprintf(stderr, "winvm: could not bind web viewer on port %d: %s\n", port, strerror(errno));
        close(s);
        return NULL;
    }
    fprintf(stderr, "winvm: interactive web viewer listening on http://127.0.0.1:%d/\n", port);
    while (!g_sig_stop) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
        handle_web_client(c);
    }
    close(s);
    return NULL;
}

/* ============================================================================
 * Built-in RFB 003.008 VNC Server (--vnc <port>)
 * ============================================================================ */

static int recv_exact(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static uint8_t keysym_to_hid(uint32_t ks) {
    if (ks >= 'a' && ks <= 'z') return (uint8_t)(0x04 + (ks - 'a'));
    if (ks >= 'A' && ks <= 'Z') return (uint8_t)(0x04 + (ks - 'A'));
    if (ks >= '1' && ks <= '9') return (uint8_t)(0x1e + (ks - '1'));
    if (ks == '0') return 0x27;
    switch (ks) {
    case 0xff08: return 0x2a; /* BackSpace */
    case 0xff09: return 0x2b; /* Tab */
    case 0xff0d: return 0x28; /* Return */
    case 0xff1b: return 0x29; /* Escape */
    case 0xffff: return 0x4c; /* Delete */
    case 0xff50: return 0x4a; /* Home */
    case 0xff51: return 0x50; /* Left */
    case 0xff52: return 0x52; /* Up */
    case 0xff53: return 0x4f; /* Right */
    case 0xff54: return 0x51; /* Down */
    case 0xff55: return 0x4b; /* Page_Up */
    case 0xff56: return 0x4e; /* Page_Down */
    case 0xff57: return 0x4d; /* End */
    case 0xffe1: return 0xe1; /* Shift_L */
    case 0xffe2: return 0xe5; /* Shift_R */
    case 0xffe3: return 0xe0; /* Control_L */
    case 0xffe4: return 0xe4; /* Control_R */
    case 0xffe9: return 0xe2; /* Alt_L */
    case 0xffea: return 0xe6; /* Alt_R */
    case 0xffeb: return 0xe3; /* Super_L (Win) */
    case 0xffec: return 0xe7; /* Super_R */
    case ' ':    return 0x2c;
    case '-': case '_': return 0x2d;
    case '=': case '+': return 0x2e;
    case '[': case '{': return 0x2f;
    case ']': case '}': return 0x30;
    case '\\': case '|': return 0x31;
    case ';': case ':': return 0x33;
    case '\'': case '"': return 0x34;
    case '`': case '~': return 0x35;
    case ',': case '<': return 0x36;
    case '.': case '>': return 0x37;
    case '/': case '?': return 0x38;
    default:
        if (ks >= 0xffbe && ks <= 0xffc9) return (uint8_t)(0x3a + (ks - 0xffbe)); /* F1..F12 */
        return 0;
    }
}

static void *vnc_client_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    char ver[12];
    if (send_bytes(fd, "RFB 003.008\n", 12) != 0 || recv_exact(fd, ver, 12) != 0) goto out;
    uint8_t sec_offer[2] = {1, 1}; /* 1 security type: None (1) */
    uint8_t sec_choice = 0, shared = 0;
    uint32_t sec_ok = 0;
    if (send_bytes(fd, sec_offer, 2) != 0 || recv_exact(fd, &sec_choice, 1) != 0 ||
        send_bytes(fd, &sec_ok, 4) != 0 || recv_exact(fd, &shared, 1) != 0)
        goto out;

    uint32_t cur_w = 1024, cur_h = 768;
    hv_winvm_display_size(&cur_w, &cur_h);
    if (cur_w == 0 || cur_h == 0) { cur_w = 1024; cur_h = 768; }

    uint8_t init_msg[24 + 12] = {
        (uint8_t)(cur_w >> 8), (uint8_t)cur_w,
        (uint8_t)(cur_h >> 8), (uint8_t)cur_h,
        32, 24, 0, 1, /* 32 bpp, depth 24, little-endian, true-color */
        0, 255, 0, 255, 0, 255, /* r/g/b max = 255 */
        16, 8, 0,               /* r_shift=16, g_shift=8, b_shift=0 (matches BGRX8888!) */
        0, 0, 0,
        0, 0, 0, 12,
        'W','i','n','d','o','w','s','P','i','x','e','l'
    };
    if (send_bytes(fd, init_msg, sizeof init_msg) != 0) goto out;

    while (!g_sig_stop) {
        uint8_t msg_type = 0;
        if (recv_exact(fd, &msg_type, 1) != 0) break;
        if (msg_type == 0) { /* SetPixelFormat */
            uint8_t skip[19];
            if (recv_exact(fd, skip, 19) != 0) break;
        } else if (msg_type == 2) { /* SetEncodings */
            uint8_t hdr[3];
            if (recv_exact(fd, hdr, 3) != 0) break;
            uint16_t nenc = (uint16_t)((hdr[1] << 8) | hdr[2]);
            for (uint16_t i = 0; i < nenc; i++) {
                uint32_t enc;
                if (recv_exact(fd, &enc, 4) != 0) goto out;
            }
        } else if (msg_type == 3) { /* FramebufferUpdateRequest */
            uint8_t req[9];
            if (recv_exact(fd, req, 9) != 0) break;
            HvWinVirt *board = hv_winvm_board();
            const uint8_t *px = NULL;
            uint32_t fw = 0, fh = 0, st = 0;
            if (board && board->fb && hv_fb_display_get_frame(board->fb, &px, &fw, &fh, &st) == 0) {
                cur_w = fw;
                cur_h = fh;
            }
            uint16_t w = (uint16_t)(cur_w > 4096 ? 4096 : cur_w);
            uint16_t h = (uint16_t)(cur_h > 4096 ? 4096 : cur_h);
            uint8_t fbu[16] = {
                0, 0, 0, 1, /* Msg 0 (FramebufferUpdate), 1 rectangle */
                0, 0, 0, 0, (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 8), (uint8_t)h,
                0, 0, 0, 0  /* Encoding 0 (Raw) */
            };
            if (send_bytes(fd, fbu, 16) != 0) break;
            if (px) {
                for (uint16_t y = 0; y < h; y++) {
                    if (send_bytes(fd, px + (size_t)y * st, (size_t)w * 4) != 0) goto out;
                }
            } else {
                uint8_t *zero = calloc(w, 4);
                for (uint16_t y = 0; y < h; y++) {
                    if (send_bytes(fd, zero, (size_t)w * 4) != 0) { free(zero); goto out; }
                }
                free(zero);
            }
        } else if (msg_type == 4) { /* KeyEvent */
            uint8_t kev[7];
            if (recv_exact(fd, kev, 7) != 0) break;
            uint32_t ks = ((uint32_t)kev[3] << 24) | ((uint32_t)kev[4] << 16) | ((uint32_t)kev[5] << 8) | kev[6];
            uint8_t hid = keysym_to_hid(ks);
            if (hid) hv_winvm_key_hid(hid, kev[0] ? 1 : 0);
        } else if (msg_type == 5) { /* PointerEvent */
            uint8_t pev[5];
            if (recv_exact(fd, pev, 5) != 0) break;
            uint8_t bmask = pev[0];
            uint16_t px_x = (uint16_t)((pev[1] << 8) | pev[2]);
            uint16_t px_y = (uint16_t)((pev[3] << 8) | pev[4]);
            uint8_t btns = (bmask & 1) | ((bmask & 4) ? 2 : 0) | ((bmask & 2) ? 4 : 0);
            int8_t wheel = (bmask & 8) ? 1 : (bmask & 16) ? -1 : 0;
            double xn = cur_w ? (double)px_x / (double)cur_w : 0.0;
            double yn = cur_h ? (double)px_y / (double)cur_h : 0.0;
            hv_winvm_tablet(xn, yn, btns, wheel);
        } else if (msg_type == 6) { /* ClientCutText */
            uint8_t hdr[7];
            if (recv_exact(fd, hdr, 7) != 0) break;
            uint32_t len = ((uint32_t)hdr[3] << 24) | ((uint32_t)hdr[4] << 16) | ((uint32_t)hdr[5] << 8) | hdr[6];
            if (len > (16u << 20)) break;
            char *cut = malloc((size_t)len + 1);
            if (!cut || recv_exact(fd, cut, len) != 0) { free(cut); break; }
            cut[len] = 0;
            hv_share_set_clipboard(cut);
            free(cut);
        }
    }
out:
    close(fd);
    return NULL;
}

static void *vnc_server_thread(void *arg) {
    int port = (int)(intptr_t)arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return NULL;
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(s, 8) != 0) {
        fprintf(stderr, "winvm: could not bind VNC server on port %d: %s\n", port, strerror(errno));
        close(s);
        return NULL;
    }
    fprintf(stderr, "winvm: VNC server listening on 127.0.0.1:%d\n", port);
    while (!g_sig_stop) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
        pthread_t ct;
        if (pthread_create(&ct, NULL, vnc_client_thread, (void *)(intptr_t)c) == 0)
            pthread_detach(ct);
        else
            close(c);
    }
    close(s);
    return NULL;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s --code <code.fd> --vars <vars.fd> [options]\n"
            "  --disk <img|hvsparse>     Primary NVMe disk image (read/write)\n"
            "  --disk1 <img|dmg>         Secondary/installer NVMe image (read-only)\n"
            "  --cpus <N>                Number of vCPUs (default: 4)\n"
            "  --ram <MB>                Guest RAM in MB (default: 4096)\n"
            "  --share <dir>             Host folder shared at http://10.0.2.4/\n"
            "  --web <port>              Start interactive HTTP/Canvas viewer (default: 8080 unless --headless)\n"
            "  --vnc <port>              Start RFB 3.8 VNC server on port (e.g. 5900)\n"
            "  --headless                Run without web viewer\n"
            "  --screenshot <png>        Save guest framebuffer PNG\n"
            "  --screenshot-after <sec>  Save screenshot after N seconds\n"
            "  --timeout <sec>           Auto-stop VM after N seconds\n"
            "  --cmd-file <path>         Tail input commands from file\n"
            "  --no-net                  Disable SLIRP networking\n"
            "  --sound                   Enable sound output\n"
            "  --stats <sec>             Print JIT statistics every N seconds\n"
            "  --log <level>             Log level (default: 0)\n",
            prog);
    exit(2);
}

int main(int argc, char **argv) {
    HvWinVmConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.ncpus = 4;
    cfg.ram_mb = 4096;

    int headless = 0;
    int web_port = 8080;
    int vnc_port = 0;
    TimerArgs targs = {NULL, 0, 0};
    const char *cmd_file = getenv("HV_INPUT_FILE");
    char share_sock_buf[512] = {0};

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) { headless = 1; continue; }
        if (!strcmp(argv[i], "--no-net"))   { cfg.no_net = 1; continue; }
        if (!strcmp(argv[i], "--sound"))    { cfg.enable_sound = 1; continue; }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) usage(argv[0]);
        if (i + 1 >= argc) usage(argv[0]);

        if (!strcmp(argv[i], "--code"))              cfg.code_fd = argv[++i];
        else if (!strcmp(argv[i], "--vars"))         cfg.vars_fd = argv[++i];
        else if (!strcmp(argv[i], "--disk"))         cfg.disk0 = argv[++i];
        else if (!strcmp(argv[i], "--disk1"))        cfg.disk1 = argv[++i];
        else if (!strcmp(argv[i], "--cpus"))         cfg.ncpus = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ram"))          cfg.ram_mb = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--share"))        cfg.share_dir = argv[++i];
        else if (!strcmp(argv[i], "--web"))          web_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vnc"))          vnc_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot"))   targs.screenshot_path = argv[++i];
        else if (!strcmp(argv[i], "--screenshot-after")) targs.screenshot_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--timeout"))      targs.timeout_sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cmd-file"))     cmd_file = argv[++i];
        else if (!strcmp(argv[i], "--stats"))        cfg.stats_interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log"))          cfg.log = atoi(argv[++i]);
        else usage(argv[0]);
    }

    if (!cfg.code_fd || !cfg.vars_fd) usage(argv[0]);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (cfg.share_dir && *cfg.share_dir) {
        snprintf(share_sock_buf, sizeof share_sock_buf, "%s/.winvm_webdav.sock", cfg.share_dir);
        cfg.share_sock = share_sock_buf;
    }

    char err[256] = {0};
    if (hv_winvm_create(&cfg, err, sizeof err) != 0) {
        fprintf(stderr, "winvm: failed to create VM: %s\n", err);
        return 1;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    if (targs.screenshot_path || targs.timeout_sec > 0) {
        pthread_t tt;
        pthread_create(&tt, NULL, timer_thread, &targs);
        pthread_detach(tt);
    }

    if (cmd_file && *cmd_file) {
        pthread_t ct;
        pthread_create(&ct, NULL, cmd_file_thread, (void *)cmd_file);
        pthread_detach(ct);
    }

    if (!headless && web_port > 0) {
        pthread_t wt;
        pthread_create(&wt, NULL, web_server_thread, (void *)(intptr_t)web_port);
        pthread_detach(wt);
    }

    if (vnc_port > 0) {
        pthread_t vt;
        pthread_create(&vt, NULL, vnc_server_thread, (void *)(intptr_t)vnc_port);
        pthread_detach(vt);
    }

    fprintf(stderr, "winvm: starting on Linux ARM64 (%d vCPUs, %llu MB RAM)...\n",
            cfg.ncpus, (unsigned long long)cfg.ram_mb);
    hv_winvm_start(0, NULL);
    int r = hv_winvm_run_cpu0();
    fprintf(stderr, "\nwinvm: stopped (%s)\n", g_sig_stop ? "interrupted" : hv_winvm_reason_name(r));
    if (targs.screenshot_path && targs.screenshot_after == 0) {
        int rc = hv_winvm_save_png(targs.screenshot_path);
        fprintf(stderr, "winvm: saved screenshot %s (rc=%d)\n", targs.screenshot_path, rc);
    }
    hv_winvm_flush_disks();
    return 0;
}
