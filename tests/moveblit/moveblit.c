#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
typedef struct { short g_x,g_y,g_w,g_h; } GRECT;
#define NAME 1
#define STORE_BACK 2
#define TOOLBAR 4
#define VSLIDE 8
#define created_for_POPUP 4
#define CS_EXITING 1
#define XAWS_OPEN 1
#define XAWS_HIDDEN 2
#define XAWS_RESIZED 4
#define XAW_TOOLBAR 0
#define XAW_VSLIDE 1
#define RDRW_WA 1
#define RDRW_EXT 2
#define RDRW_ALL 3
#define AMQ_NORM 0
#define QMF_CHKDUP 0
#define WM_MOVED 28
#define WTF_EXTRA_ISLIST 1
struct xa_client { int status; };
struct widget_tree { void *extra; int flags; };
typedef struct scroll_info { struct xa_window *wi; struct scroll_info *next; } SCROLL_INFO;
typedef struct xa_widget { struct { struct widget_tree *wt; } stuff; GRECT ar; } XA_WIDGET;
struct xa_rect_list { struct xa_rect_list *next; GRECT r; };
struct xa_window { struct xa_window *prev; struct xa_client *owner; int active_widgets, window_status, nolist, frame, dial, fluent, handle, id;
  GRECT r, wa; struct { struct xa_rect_list *start, *next; } rect_list; XA_WIDGET widgets[2];
  void (*send_message)(int, struct xa_window *, void *, int, int, int, ...); };
#define APJ_SHAPE_MAX 40
struct build_rl_parms { int (*getnxtrect)(struct build_rl_parms *p); GRECT *area; GRECT *next_r; void *ptr1; short nshape, ishape; GRECT shape[APJ_SHAPE_MAX]; };
#define SW 1920
#define SH 1080
static struct { GRECT r; } screen = {{0,0,SW,SH}};
static struct { int menu_bar; } cfg = {1};
static struct xa_window menuw, *menu_window = &menuw;
static struct xa_window rootw, *root_window = &rootw;
static struct { struct { struct xa_window *last; } open_nlwindows; } S;
static int nalloc;
static void *km(size_t n){ nalloc++; return malloc(n); }
static void kf(void *p){ nalloc--; free(p); }
#define kmalloc km
#define kfree kf
#define DIAGS(x)
short apj_window_fluent(struct xa_window *w){ return w->fluent; }
short apj_round_radius(void);
short apj_round_steps(short r, const short **inset);
short apj_corner_steps(struct xa_window *wind, const short **inset);
short apj_corner_rows(struct xa_window *wind, const short **inset, short *nt, short *nb);
bool xa_rect_clip(const GRECT *s, const GRECT *d, GRECT *r);
#include "rl.inc"
bool xa_rect_clip(const GRECT *s, const GRECT *d, GRECT *r)
{
	if (s->g_w > 0 && s->g_h > 0 && d->g_w > 0 && d->g_h > 0) {
		const short w1 = s->g_x + s->g_w, w2 = d->g_x + d->g_w, h1 = s->g_y + s->g_h, h2 = d->g_y + d->g_h;
		r->g_x = s->g_x > d->g_x ? s->g_x : d->g_x; r->g_y = s->g_y > d->g_y ? s->g_y : d->g_y;
		r->g_w = (w1 < w2 ? w1 : w2) - r->g_x; r->g_h = (h1 < h2 ? h1 : h2) - r->g_y;
		return ((r->g_w > 0) && (r->g_h > 0));
	}
	return false;
}
static unsigned int scr[SH][SW], tmp[SH][SW];
static unsigned int content(struct xa_window *w, int x, int y){ return ((unsigned)w->id<<24) | ((unsigned)(y-w->r.g_y)<<12) | (unsigned)(x-w->r.g_x); }
static void paintw(struct xa_window *w, struct xa_rect_list *l, const GRECT *clip){
  for(;l;l=l->next){ GRECT c; if(!xa_rect_clip(&l->r,&screen.r,&c)) continue; if(clip && !xa_rect_clip(&c,clip,&c)) continue;
    for(int y=c.g_y;y<c.g_y+c.g_h;y++) for(int x=c.g_x;x<c.g_x+c.g_w;x++) scr[y][x]=content(w,x,y); } }
static int blits;
struct vapi { void (*form_copy)(GRECT *, GRECT *); };
static void form_copy(GRECT *s, GRECT *d){
  blits++;
  for(int y=0;y<s->g_h;y++) for(int x=0;x<s->g_w;x++){ int sx=s->g_x+x, sy=s->g_y+y;
    tmp[y][x] = (sx>=0&&sy>=0&&sx<SW&&sy<SH) ? scr[sy][sx] : 0xDEAD; }
  for(int y=0;y<s->g_h;y++) for(int x=0;x<s->g_w;x++){ int dx=d->g_x+x, dy=d->g_y+y;
    if(dx>=0&&dy>=0&&dx<SW&&dy<SH) scr[dy][dx]=tmp[y][x]; }
}
static struct vapi va={form_copy}, *xa_vdiapi=&va;
static void hidem(void){} static void showm(void){}
static XA_WIDGET *get_widget(struct xa_window *w, int i){ return &w->widgets[i]; }
static void generate_redraws(int lock, struct xa_window *w, GRECT *r, int flags){ (void)lock;(void)flags; paintw(w, w->rect_list.start, r); }
static void freel(struct xa_rect_list *l){ while(l){ struct xa_rect_list *n=l->next; kf(l); l=n; } }

static struct xa_rect_list *mk(struct xa_window *w){ struct build_rl_parms p; memset(&p,0,sizeof p); GRECT area=w->r; p.getnxtrect=nextwind_rect; p.area=&area; p.ptr1=w->prev; p.nshape=apj_shape_rects(w,p.shape); p.ishape=0; return build_rect_list(&p); }

#include "order.inc"
static void do_move(struct xa_window *wind, GRECT *new)
{
	short dir, resize, xmove, ymove, wlock = 0; bool blit = true, only_wa = false;
	struct xa_rect_list *oldrl, *orl, *newrl, *brl, *prev, *next, *rrl, *nrl;
	GRECT bs, bd, old, wa, oldw;
	old = wind->r; oldw = wind->wa;
	wind->wa.g_x += new->g_x - old.g_x; wind->wa.g_y += new->g_y - old.g_y;
	wind->r = *new; wa = wind->wa;
	xmove = new->g_x - old.g_x; ymove = new->g_y - old.g_y; resize = 0;
	oldrl = wind->rect_list.start;
	newrl = mk(wind);
	wind->rect_list.start = newrl;
#include "blit.inc"
}

/* check the moved window: every on-screen pixel it owns shows its content */
static long check(struct xa_window *w){
  long bad=0; for(struct xa_rect_list *l=w->rect_list.start;l;l=l->next){ GRECT c; if(!xa_rect_clip(&l->r,&screen.r,&c)) continue;
    for(int y=c.g_y;y<c.g_y+c.g_h;y++) for(int x=c.g_x;x<c.g_x+c.g_w;x++) if(scr[y][x]!=content(w,x,y)) bad++; }
  return bad;
}

static int ytop = 40;
static int run(const char *name, struct xa_window *W, int steps, int seed, int maxd, int *worst)
{
  long fails=0; srand(seed);
  memset(scr,0,sizeof scr);
  W->rect_list.start=mk(W); paintw(W,W->rect_list.start,NULL);
  for(int s=0;s<steps;s++){
    short dx=(rand()%(2*maxd+1))-maxd, dy=(rand()%(2*maxd+1))-maxd;
    GRECT n=W->r; n.g_x+=dx; n.g_y+=dy;
    if(n.g_x<0||n.g_y<ytop||n.g_x+n.g_w>SW||n.g_y+n.g_h>SH-60){ n=W->r; n.g_x-=dx; n.g_y-=dy; if(n.g_x<0||n.g_y<ytop||n.g_x+n.g_w>SW||n.g_y+n.g_h>SH-60) continue; }
    GRECT o=W->r; long b0=check(W);
    do_move(W,&n);
    long b=check(W);
    if(b>b0){ fails++; if(fails<=5) printf("  %s step %d: move (%d,%d)->(%d,%d) d=(%d,%d): %ld bad px (was %ld)\n",name,s,o.g_x,o.g_y,n.g_x,n.g_y,dx,dy,b,b0); }
  }
  long b=check(W); printf("%s: %d moves, %ld bad moves, %ld bad pixels at end\n", name, steps, fails, b);
  freel(W->rect_list.start); W->rect_list.start=NULL;
  return fails||b;
}

int main(void){
  struct xa_client cl={0};
  menuw.r=(GRECT){0,0,SW,30};
  root_window->r=screen.r; root_window->wa=(GRECT){0,30,SW,SH-30};
  struct xa_window A={0}, T={0};
  A.owner=T.owner=&cl; A.id=1; T.id=2;
  A.active_widgets=NAME; A.window_status=XAWS_OPEN; A.frame=2; A.fluent=1;
  A.r=(GRECT){422,300,596,476};
  A.wa=(GRECT){A.r.g_x+2, A.r.g_y+60, A.r.g_w-16, A.r.g_h-60-14};   /* title+info, v+h scrollbars */
  int rc=0, worst=0;
  const short *in; short nt,nb; apj_corner_rows(&A,&in,&nt,&nb); GRECT sh[APJ_SHAPE_MAX];
  printf("corner rows top %d bottom %d, shape rects %d\n", nt, nb, apj_shape_rects(&A,sh));
  rc|=run("alone, small moves", &A, 3000, 1, 12, &worst);
  rc|=run("alone, large moves", &A, 3000, 2, 120, &worst);
  A.fluent=0;
  rc|=run("square window, small moves", &A, 3000, 3, 12, &worst);
  A.fluent=1;
  /* a square window above (the taskbar strip region) */
  T.active_widgets=0; T.window_status=XAWS_OPEN; T.frame=-1; T.r=(GRECT){700,500,300,120}; T.wa=T.r; A.prev=&T;
  rc|=run("under a square window, small moves", &A, 3000, 4, 12, &worst);
  /* a rounded window above */
  T.active_widgets=NAME; T.frame=2; T.fluent=1; T.r=(GRECT){800,450,400,250}; T.wa=(GRECT){T.r.g_x+2,T.r.g_y+36,T.r.g_w-16,T.r.g_h-36-14};
  rc|=run("under a rounded window, small moves", &A, 3000, 5, 12, &worst);
  rc|=run("under a rounded window, large moves", &A, 3000, 6, 90, &worst);
  A.prev=NULL; ytop=30; A.r.g_y=40; A.wa.g_y=A.r.g_y+60;
  rc|=run("alone, along the menu bar", &A, 3000, 7, 8, &worst);
  printf("leaked allocations: %d\n", nalloc);
  return rc;
}
