#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
typedef struct { short g_x,g_y,g_w,g_h; } GRECT;
#define NAME 1
#define STORE_BACK 2
#define created_for_POPUP 4
#define CS_EXITING 1
#define XAWS_OPEN 1
#define XAWS_HIDDEN 2
struct xa_client { int status; };
struct xa_window { struct xa_window *prev; struct xa_client *owner; int active_widgets, window_status, nolist, frame, dial, fluent; GRECT r, wa; };
struct xa_rect_list { struct xa_rect_list *next; GRECT r; };
#define APJ_SHAPE_MAX 40
struct build_rl_parms { int (*getnxtrect)(struct build_rl_parms *p); GRECT *area; GRECT *next_r; void *ptr1; short nshape, ishape; GRECT shape[APJ_SHAPE_MAX]; };
static struct { GRECT r; } screen = {{0,0,1920,1080}};
static struct { int menu_bar; } cfg = {0};
static struct xa_window *menu_window = NULL;
static struct xa_window rootw, *root_window = &rootw;
static struct { struct { struct xa_window *last; } open_nlwindows; } S;
#define kmalloc malloc
#define kfree free
#define DIAGS(x)
short apj_window_fluent(struct xa_window *w){ return w->fluent; }
short apj_round_radius(void);
short apj_round_steps(short r, const short **inset);
short apj_corner_steps(struct xa_window *wind, const short **inset);
short apj_corner_rows(struct xa_window *wind, const short **inset, short *nt, short *nb);
bool xa_rect_clip(const GRECT *s, const GRECT *d, GRECT *r);
#include "rl.inc"
bool
xa_rect_clip(const GRECT *s, const GRECT *d, GRECT *r)
{
	if (s->g_w > 0 && s->g_h > 0 && d->g_w > 0 && d->g_h > 0)
	{
		const short w1 = s->g_x + s->g_w;
		const short w2 = d->g_x + d->g_w;
		const short h1 = s->g_y + s->g_h;
		const short h2 = d->g_y + d->g_h;

		r->g_x = s->g_x > d->g_x ? s->g_x : d->g_x;	//max(s->x, d->g_x);
		r->g_y = s->g_y > d->g_y ? s->g_y : d->g_y;	//max(s->y, d->g_y);
		r->g_w = (w1 < w2 ? w1 : w2) - r->g_x; 	//min(w1, w2) - d->g_x;
		r->g_h = (h1 < h2 ? h1 : h2) - r->g_y;	//min(h1, h2) - d->g_y;

		return ((r->g_w > 0) && (r->g_h > 0));
	}
	else
		return false;
}


static unsigned short own[1080][1920];
static void paint(struct xa_rect_list *l, int id){ for(;l;l=l->next) for(int y=l->r.g_y;y<l->r.g_y+l->r.g_h;y++) for(int x=l->r.g_x;x<l->r.g_x+l->r.g_w;x++){ if(own[y][x]){printf("OVERLAP at %d,%d (%d and %d)\n",x,y,own[y][x],id); exit(1);} own[y][x]=id; } }
static struct xa_rect_list *mk(struct xa_window *w){ struct build_rl_parms p; memset(&p,0,sizeof p); p.getnxtrect=nextwind_rect; p.area=&w->r; p.ptr1=w->prev; p.nshape=apj_shape_rects(w,p.shape); p.ishape=0; return build_rect_list(&p); }
static int count(struct xa_rect_list *l){int n=0; for(;l;l=l->next)n++; return n;}
/* v api stub for outline */
struct vapi { void (*line)(void*,short,short,short,short,short); };
static char pix[1080][1920];
static void line(void *v, short x1, short y1, short x2, short y2, short c){ (void)v;(void)c; for(int x=x1;x<=x2;x++) pix[y1][x]='#'; (void)y2; }
int main(void){
  struct xa_client cl={0};
  root_window->r=screen.r; root_window->wa=(GRECT){0,30,1920,1010};
  struct xa_window A={0},B={0},D={0};
  /* stacking: A top, B below, D = desktop-ish full screen window at bottom (square) */
  A.owner=B.owner=D.owner=&cl; A.active_widgets=B.active_widgets=NAME; A.window_status=B.window_status=D.window_status=XAWS_OPEN;
  A.frame=B.frame=2; D.frame=-1; A.fluent=B.fluent=1; D.fluent=0;
  A.r=(GRECT){400,300,600,400}; B.r=(GRECT){200,200,500,300}; D.r=screen.r;
  A.prev=NULL; B.prev=&A; D.prev=&B;
  A.wa=(GRECT){A.r.g_x+2, A.r.g_y+34, A.r.g_w-16, A.r.g_h-34-14};   /* title + h/v scrollbars */
  B.wa=(GRECT){B.r.g_x+2, B.r.g_y+34, B.r.g_w-4,  B.r.g_h-34-2};    /* no scrollbars */
  const short *in; short n=apj_corner_steps(&A,&in);
  printf("r=8 steps n=%d:",n); for(int k=0;k<n;k++) printf(" %d",in[k]); printf("\n");
  GRECT sh[APJ_SHAPE_MAX]; short ns=apj_shape_rects(&A,sh);
  printf("shape rects %d\n", ns);
  struct xa_rect_list *la=mk(&A), *lb=mk(&B), *ld=mk(&D);
  printf("rect counts: A %d  B %d  D %d\n", count(la),count(lb),count(ld));
  paint(la,1); paint(lb,2); paint(ld,3);
  long holes=0; for(int y=0;y<1080;y++) for(int x=0;x<1920;x++) if(!own[y][x]) holes++;
  printf("pixels owned by nobody: %ld\n", holes);
  /* corner checks */
  printf("A top-left corner pixel (400,300) owner=%d (expect 2 = B beneath)\n", own[300][400]);
  printf("A (405,300) owner=%d (expect 1)\n", own[300][405]);
  printf("B bottom-right corner (699,499) owner=%d (expect 1: A covers)\n", own[499][699]);
  printf("B top-left corner (200,200) owner=%d (expect 3 desktop)\n", own[200][200]);
  /* ASCII of A's top-left 10x10: '.' not A, 'A' A-owned, '#' outline drawn inside A */
  struct { void *api; } vs; struct vapi va={line}; (void)vs;
  struct xa_window *wind=&A; struct { struct vapi *api; } vv={&va}; struct { struct vapi *api; } *v=&vv;
  struct window_colours { short frame_col; } wcx={1}; struct { void *colours; } dummy={&wcx}; (void)dummy;
  /* emulate rings + outline, clipped by A ownership */
  for(int i=0;i<A.frame;i++){ GRECT r={A.r.g_x+i,A.r.g_y+i,A.r.g_w-2*i,A.r.g_h-2*i};
    for(int x=r.g_x;x<r.g_x+r.g_w;x++){pix[r.g_y][x]='#';pix[r.g_y+r.g_h-1][x]='#';}
    for(int y=r.g_y;y<r.g_y+r.g_h;y++){pix[y][r.g_x]='#';pix[y][r.g_x+r.g_w-1]='#';} }
  {
    const short *in2; short nt,nb,k; apj_corner_rows(wind,&in2,&nt,&nb); short x1=wind->r.g_x, x2=wind->r.g_x+wind->r.g_w-1, y1=wind->r.g_y, y2=wind->r.g_y+wind->r.g_h-1;
    for(k=0;k<nt||k<nb;k++){ short a=in2[k]; short b=(k?in2[k-1]-1:in2[k])+wind->frame-1; if(b<a+wind->frame-1)b=a+wind->frame-1;
      if(k<nt){line(v,x1+a,y1+k,x1+b,y1+k,1); line(v,x2-b,y1+k,x2-a,y1+k,1);}
      if(k<nb){line(v,x1+a,y2-k,x1+b,y2-k,1); line(v,x2-b,y2-k,x2-a,y2-k,1);} } }
  for(int corner=0;corner<2;corner++){
   printf(corner?"A bottom-right 12x12:\n":"A top-left 12x12:\n");
   for(int y=0;y<12;y++){ int yy= corner? A.r.g_y+A.r.g_h-12+y : A.r.g_y+y; for(int x=0;x<12;x++){ int xx= corner? A.r.g_x+A.r.g_w-12+x : A.r.g_x+x;
      putchar(own[yy][xx]!=1?'.':(pix[yy][xx]=='#'?'#':'a')); } putchar('\n'); }
  }
  /* B on top of the desktop, alone: how many rects does its WORK AREA get? */
  {
    struct xa_window B2=B; B2.prev=NULL;
    struct xa_rect_list *l=mk(&B2); int n=0, pix=0;
    for(;l;l=l->next){ GRECT c; short x1=l->r.g_x>B2.wa.g_x?l->r.g_x:B2.wa.g_x, y1=l->r.g_y>B2.wa.g_y?l->r.g_y:B2.wa.g_y;
      short x2=(l->r.g_x+l->r.g_w<B2.wa.g_x+B2.wa.g_w)?l->r.g_x+l->r.g_w:B2.wa.g_x+B2.wa.g_w;
      short y2=(l->r.g_y+l->r.g_h<B2.wa.g_y+B2.wa.g_h)?l->r.g_y+l->r.g_h:B2.wa.g_y+B2.wa.g_h;
      if(x2>x1&&y2>y1){n++; pix+=(x2-x1)*(y2-y1);} (void)c; }
    printf("window without bottom scrollbar: work area split into %d rect(s), %d of %d px (expect 1, all)\n", n, pix, B2.wa.g_w*B2.wa.g_h);
    const short *i3; short nt,nb; apj_corner_rows(&B2,&i3,&nt,&nb); printf("B carved rows top %d bottom %d; A: ", nt, nb);
    apj_corner_rows(&A,&i3,&nt,&nb); printf("top %d bottom %d\n", nt, nb);
  }
  return 0;
}
