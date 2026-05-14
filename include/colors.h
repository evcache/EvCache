#ifndef COLORS_H
#define COLORS_H

#define RST  "\x1b[0m" // reset

// colors
#define BLK  "\x1b[30m"
#define RED  "\x1b[31m"
#define GRN  "\x1b[32m"
#define YLW  "\x1b[33m"
#define BLUE "\033[94m"
#define CYN  "\x1b[96m"        // bright cyan
#define PINK "\x1b[35m"
#define GRY  "\x1b[38;5;247m"

// printing statuses
#define SUC   GRN  "[+] " RST          // success
#define ERR   RED  "[-] " RST          // error
#define WRN   YLW  "[!] " RST          // warning
#define IMP   PINK "[IMPORTANT] " RST  // important
#define INFO  BLUE "[INFO] " RST       // info
#define NOTE  CYN  "[NOTE] " RST       // note
#define V1    GRY "[V1] " RST          // verbose level 1
#define V2    GRY "[V2] " RST
#define V3    GRY "[V3] " RST
#define D1    GRY "[D1] " RST          // debug level 1
#define D2    GRY "[D2] " RST
#define D3    GRY "[D3] " RST
#define LR    PINK "\n[!!] " RST "LINE REACHED " PINK "[!!]" RST "\n" // for debugging

#endif // COLORS_H

