// Calls from the guest program to the runner, made with `svc #0` and the number in x8.
// Numbers below 0x4000 are ordinary Linux system calls.
#pragma once
#define HC_BASE          0x4000
#define HC_PRESENT       0x4001  // show the finished frame -> 1 if the display's refresh already paced it (do not sleep)
#define HC_POLL_EVENT    0x4002  // (out struct HcEvent*) -> 1 if an event was written
#define HC_WINDOW_SIZE   0x4003  // (out int w*, out int h*)
#define HC_HTTP_START    0x4010  // (struct HcHttp*) -> handle; blocks the calling guest thread until done
#define HC_HTTP_READ     0x4011  // (handle, headers buf, body buf)
#define HC_HTTP_FREE     0x4012  // (handle)
#define HC_AUDIO_OPEN    0x4020  // (rate, channels, bits) -> id
#define HC_AUDIO_WRITE   0x4021  // (id, ptr, bytes)
#define HC_AUDIO_QUEUED  0x4022  // (id) -> milliseconds still waiting to be played
#define HC_AUDIO_CLOSE   0x4023  // (id)
#define HC_AUDIO_CTL     0x4024  // (id, what, value): 0 = playing on/off, 1 = volume in millibels, 2 = clear
#define HC_IMAGE         0x4030  // (base, size, path) tells the runner where the game library is, for crash reports
#define HC_TEXT_INPUT    0x4031  // (title, initial text, out buf, cap) -> 1 if confirmed
#define HC_OPEN_URL      0x4032  // (url)
#define HC_AES          0x4033  // (flags, key16, iv16 or 0, buf, len): bit 0 decrypt, bit 1 CBC; in place
#define HC_PAGE_OK      0x4034  // (address) -> 1 if that guest page is mapped (debug tracing)
#define HC_MD5          0x4035  // (data, length, out 16 bytes)
#define HC_MESSAGE      0x4036  // (title, text) shows a message box
#define HC_CLIPBOARD    0x4037  // (text) copies to the clipboard
#define HC_THREAD_URGENT 0x4038 // (on) marks the calling thread as a sound thread: scheduled ahead of the others
#define HC_DECODE_LINES  0x4039  // (flags, key16, iv16 or 0, text, len): a table file decoded in place, line by line (Base64, AES as HC_AES,
                                 // padding removed, lines joined with \n) -> new length, or -1 if a line does not decrypt
#define HC_WEB_SHOW      0x4040  // (url, post fields or "", x, y, w, h in game pixels from the top left) shows an in-game web page
#define HC_WEB_VISIBLE   0x4041  // (on) takes the page off the screen, or puts it back
#define HC_WEB_STATE     0x4042  // -> 0 nothing on screen, 1 loading, 2 showing, 3 could not be loaded
#define HC_WEB_PARAM     0x4043  // (out buf, cap) -> 1 with the text of a tapped ffbecall:// link, else 0
#define HC_GL_BASE       0x5000  // + index into the GL name list

struct HcEvent { int type; int id; float x, y; int key; int pad; };
enum { HEV_NONE, HEV_TOUCH_DOWN, HEV_TOUCH_MOVE, HEV_TOUCH_UP, HEV_KEY_DOWN, HEV_KEY_UP, HEV_QUIT, HEV_PAUSE, HEV_RESUME,
       HEV_TEXT /* key = one typed character (Unicode) */ };
// Key codes are Android's: 4 = BACK, 66 = ENTER, 67 = DEL (Backspace).

struct HcHttp {
    unsigned long long url, method, headers;   // guest pointers to C strings (headers: "K: V\r\n" lines, may be 0)
    unsigned long long body, body_len;
    unsigned long long save_path;              // guest pointer to a path: stream the reply body to this file instead of memory
    // filled by the runner
    int status, failed;
    unsigned long long headers_len, body_len_out;
    unsigned int crc32, pad;
};
