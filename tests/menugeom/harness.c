#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#define G_BOX 20
#define G_IBOX 25
#define G_STRING 28
#define G_TITLE 32
#define OF_LASTOB 0x20
#define OF_HIDETREE 0x80
#define OS_DISABLED 0x08
typedef struct { short ob_next, ob_head, ob_tail; unsigned short ob_type, ob_flags, ob_state; long ob_spec; short ob_x, ob_y, ob_width, ob_height; } OBJECT;
typedef struct { short g_x,g_y,g_w,g_h; } GRECT;
typedef struct { short apj_menu; void *apj_mgeom; OBJECT *tree; GRECT area; } XA_TREE;
struct xa_window { int dummy; };
struct xa_client;
union spec { char *free_string; };
static char *strtab[4096]; /* ob_spec index -> string */
static union spec specbuf;
static union spec *object_get_spec(OBJECT *o){ specbuf.free_string = strtab[o->ob_spec]; return &specbuf; }
static struct { short c_max_w, c_max_h; } screen = {12, 24};
static struct { short menu_layout; } cfg;
#define MONO 0
static int fluent = 1;
static int client_apj_chrome(void *c){ return fluent; }
static struct { void *Aes; } C;
#define kmalloc malloc
#define kfree free
static void wt_menu_area(XA_TREE *wt){}
static struct xa_window rootw; static struct xa_window *root_window=&rootw;
static short barh = 36;
struct xa_widget { GRECT r; };
static struct xa_widget mw; 
#define get_widget(w,x) (&mw)
#define get_menu_widg() (&mw)
#define DIAG(x)
static unsigned long menu_colors(void){return 0;}
static void object_set_spec(OBJECT *o, unsigned long v){}
static void menu_spec(OBJECT *t, int i){}
#include "block.inc"
#include "fix.inc"
static unsigned char *F; static long FL;
static short be16(long o){ return (short)((F[o]<<8)|F[o+1]); }
static long be32(long o){ return ((long)F[o]<<24)|((long)F[o+1]<<16)|((long)F[o+2]<<8)|F[o+3]; }
static short fixc(short v, short unit){ return (short)((v & 0xff) * unit + (signed char)((v >> 8) & 0xff)); }
static void dump(OBJECT *t, int n, const char *tag){
  printf("--- %s\n", tag);
  for (int i=0;i<n;i++){ int ty=t[i].ob_type&0xff; if(ty!=G_TITLE && ty!=G_BOX && ty!=G_IBOX && ty!=G_STRING) continue;
    printf("%3d %-6s x=%4d y=%4d w=%4d h=%3d %s '%s'\n", i, ty==G_TITLE?"TITLE":ty==G_BOX?"BOX":ty==G_IBOX?"IBOX":"STR", t[i].ob_x,t[i].ob_y,t[i].ob_width,t[i].ob_height,
      (t[i].ob_state&OS_DISABLED)?"D":" ", (ty==G_TITLE||ty==G_STRING)?strtab[t[i].ob_spec]:""); }
}
int main(void){
  FILE *f=fopen("desktop.rsc","rb"); F=malloc(1<<20); FL=fread(F,1,1<<20,f);
  long obj=(unsigned short)be16(2), trindex=(unsigned short)be16(18); int ntree=be16(22);
  for (int tr=0; tr<ntree; tr++){
    long root = be32(trindex+4*tr); int ri=(root-obj)/24;
    long o = obj + ri*24;
    int bar = be16(o+2); if (bar<0) continue;
    int titles = be16(obj+(ri+bar)*24+2); if (titles<0) continue;
    int t1 = be16(obj+(ri+titles)*24+2); if (t1<0) continue;
    if ((be16(obj+(ri+t1)*24+6)&0xff)!=G_TITLE) continue;
    int n=0; while(!(be16(obj+(ri+n)*24+8)&OF_LASTOB)) n++; n++;
    OBJECT *t=calloc(n,sizeof(OBJECT)), *pristine=calloc(n,sizeof(OBJECT)); int ns=0;
    for(int i=0;i<n;i++){ long p=obj+(ri+i)*24;
      t[i].ob_next=be16(p); t[i].ob_head=be16(p+2); t[i].ob_tail=be16(p+4);
      t[i].ob_type=be16(p+6); t[i].ob_flags=be16(p+8); t[i].ob_state=be16(p+10);
      long sp=be32(p+12); int ty=t[i].ob_type&0xff;
      if(ty==G_STRING||ty==G_TITLE){ strtab[++ns]=(char*)F+sp; t[i].ob_spec=ns; } else t[i].ob_spec=0;
      t[i].ob_x=fixc(be16(p+16),12); t[i].ob_y=fixc(be16(p+18),24); t[i].ob_width=fixc(be16(p+20),12); t[i].ob_height=fixc(be16(p+22),24);
    }
    memcpy(pristine,t,n*sizeof(OBJECT));
    printf("menu tree %d, %d objects\n", tr, n);
    XA_TREE wt={0}; wt.tree=t; mw.r.g_w=1920;
    fluent=1; mw.r.g_h=36; fix_menu(&wt, root_window); dump(t,n,"FLUENT"); printf("apj_menu=%d saved=%p\n", wt.apj_menu, wt.apj_mgeom);
    /* reinstall while fluent: must be idempotent */
    OBJECT *once=calloc(n,sizeof(OBJECT)); memcpy(once,t,n*sizeof(OBJECT));
    fix_menu(&wt, root_window); printf("re-fix idempotent: %s\n", memcmp(once,t,n*sizeof(OBJECT))?"NO":"yes");
    /* theme off */
    fluent=0; mw.r.g_h=26; fix_menu(&wt, root_window);
    XA_TREE w2={0}; w2.tree=pristine; fix_menu(&w2, root_window);
    printf("theme off == stock fix of pristine: %s; apj_menu=%d saved=%p\n", memcmp(pristine,t,n*sizeof(OBJECT))?"NO":"yes", wt.apj_menu, wt.apj_mgeom);
    dump(t,n,"STOCK (first 12)");
  }
  return 0;
}
