#include "../apps/pivid-tx/src/tx_csv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
#define EXPECT(c,m) do{ if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,m);g_failures++;} }while(0)

static long count_lines(const char *path){
    FILE *f = fopen(path,"r"); if(!f) return -1;
    long n=0; int c;
    while((c=fgetc(f))!=EOF){ if(c=='\n') n++; }
    fclose(f); return n;
}

int main(void){
    const char *path = "/tmp/test_tx_csv.csv";
    remove(path);

    FILE *a = txcsv_acquire(path);
    EXPECT(a != NULL, "first acquire opens the file");
    FILE *b = txcsv_acquire(path);
    EXPECT(b == a, "second acquire returns the same FILE*");

    /* First acquire wrote exactly one header line; no duplicate from the 2nd. */
    fflush(a);
    EXPECT(count_lines(path) == 1, "exactly one header line after two acquires");

    fprintf(a, "OCC,1,2,3,4,5,6,7\n");
    fprintf(b, "AQM,8,IDR,gdr,0,KEEP,peak,9,2,3,4,5\n");
    fflush(a);
    EXPECT(count_lines(path) == 3, "header + two data rows");

    txcsv_release();                    /* refs 2 -> 1: file stays open */
    fprintf(a, "OCC,9,9,9,9,9,9,9\n");   /* still writable */
    fflush(a);
    EXPECT(count_lines(path) == 4, "file still open after one release");

    txcsv_release();                    /* refs 1 -> 0: file closed */

    /* Re-acquire after full release truncates and re-writes the header. */
    FILE *c = txcsv_acquire(path);
    EXPECT(c != NULL, "re-acquire after full release");
    fflush(c);
    EXPECT(count_lines(path) == 1, "re-acquire truncated -> one header line");
    txcsv_release();

    if(g_failures){fprintf(stderr,"test_tx_csv: %d failure(s)\n",g_failures);return 1;}
    printf("test_tx_csv: OK\n"); return 0;
}
