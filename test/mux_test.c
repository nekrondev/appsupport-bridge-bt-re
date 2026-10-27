/* mux_test.c — unit tests for the mux/virtualization logic (mux.c), off-device.
 *
 * Compiles mux.c with stubbed I/O (gbinder_svc_deliver / vhci_write / hci_channel_write)
 * and feeds it crafted HCI packets, asserting what reaches each consumer / the controller.
 * Validates: scan-enable arbitration, resolving-/accept-list virtualization, ext->legacy
 * advertising-report conversion, vendor APCF filter hiding, ext-create-conn quirk, connection
 * ownership routing, and command-complete demux.
 *
 *   build:  cc test/mux_test.c -Isrc <glib+gbinder/glibutil include flags> -lglib-2.0 -o mux_test
 */
#include <stdio.h>
#include <string.h>
#include "bridge.h"

/* ---- capture stubs for the functions mux.c calls ---- */
static uint8_t cont_buf[1024]; static int cont_len, cont_n;
static uint8_t bz_buf[1024];   static int bz_len,   bz_n;
static uint8_t ctrl_buf[1024]; static int ctrl_len, ctrl_n;

void gbinder_svc_deliver(App *a, const uint8_t *b, gsize l)
{ (void)a; memcpy(cont_buf, b, l); cont_len = (int)l; cont_n++; }
gboolean vhci_write(VhciChannel *v, const uint8_t *b, gsize l, GError **e)
{ (void)v; (void)e; memcpy(bz_buf, b, l); bz_len = (int)l; bz_n++; return TRUE; }
gboolean hci_channel_write(HciChannel *c, const uint8_t *b, gsize l, GError **e)
{ (void)c; (void)e; memcpy(ctrl_buf, b, l); ctrl_len = (int)l; ctrl_n++; return TRUE; }

#include "mux.c"   /* pull in the statics + mux_init/submit/on_event */

static App app;
static void reset_caps(void){ cont_len=bz_len=ctrl_len=-1; cont_n=bz_n=ctrl_n=0; }
static void setup(void){ mux_free(&app); memset(&app,0,sizeof app); mux_init(&app);
                         app.vhci=(VhciChannel*)1; app.hci0=(HciChannel*)1; reset_caps(); }
static unsigned op_of(const uint8_t*b){ return b[1] | (b[2]<<8); }

static int tests, fails;
#define CHECK(cond,msg) do{ tests++; if(cond) printf("  ok   %s\n",msg); \
                            else { fails++; printf("  FAIL %s\n",msg);} }while(0)

/* feed a status-only Command Complete for opcode (clears inflight + credits) */
static void feed_cc(unsigned op){ uint8_t e[7]={0x04,0x0e,0x04,0x01,op&0xff,op>>8,0x00};
                                  mux_on_event(&app,e,sizeof e); }

int main(void)
{
    printf("== scan-enable arbitration ==\n");
    setup();
    uint8_t sc_en[]={0x01,0x0c,0x20,0x02,0x01,0x00};
    mux_submit(&app, ORIG_CONTAINER, sc_en, sizeof sc_en);
    CHECK(ctrl_n==1 && op_of(ctrl_buf)==0x200c, "first enable forwarded to controller");
    feed_cc(0x200c); reset_caps();
    mux_submit(&app, ORIG_BLUEZ, sc_en, sizeof sc_en);
    CHECK(ctrl_n==0 && bz_n==1, "second enable synthesized, not re-forwarded");
    reset_caps();
    uint8_t sc_dis[]={0x01,0x0c,0x20,0x02,0x00,0x00};
    mux_submit(&app, ORIG_BLUEZ, sc_dis, sizeof sc_dis);
    CHECK(ctrl_n==0, "disable ignored while the other side still scans");
    reset_caps();
    mux_submit(&app, ORIG_CONTAINER, sc_dis, sizeof sc_dis);
    CHECK(ctrl_n==1 && op_of(ctrl_buf)==0x200c && ctrl_buf[4]==0, "final disable forwarded");

    printf("== resolving-list virtualization ==\n");
    setup();
    uint8_t add[]={0x01,0x27,0x20,0x07,0x00,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
    mux_submit(&app, ORIG_CONTAINER, add, sizeof add);
    CHECK(ctrl_n==1 && op_of(ctrl_buf)==0x2027, "first add forwarded");
    feed_cc(0x2027); reset_caps();
    mux_submit(&app, ORIG_BLUEZ, add, sizeof add);
    CHECK(ctrl_n==0 && bz_n==1, "duplicate add synthesized (ref-counted)");
    reset_caps();
    uint8_t rem[]={0x01,0x28,0x20,0x07,0x00,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
    mux_submit(&app, ORIG_CONTAINER, rem, sizeof rem);
    CHECK(ctrl_n==0 && cont_n==1, "remove while other holds -> synthesized");
    reset_caps();
    mux_submit(&app, ORIG_BLUEZ, rem, sizeof rem);
    CHECK(ctrl_n==1 && op_of(ctrl_buf)==0x2028, "last remove forwarded to controller");

    printf("== ext->legacy advertising-report conversion ==\n");
    setup();
    app.scan_want[ORIG_CONTAINER]=1; app.scan_type[ORIG_CONTAINER]=1;
    uint8_t er[64]; int p=0;
    er[p++]=0x04; er[p++]=0x3e; er[p++]=0; er[p++]=0x0d; er[p++]=1;    /* subevent 0x0d, 1 report */
    er[p++]=0x13; er[p++]=0x00;                                        /* event_type: legacy|conn|scan */
    er[p++]=0x00;                                                      /* addr_type */
    uint8_t mac[6]={0x11,0x22,0x33,0x44,0x55,0x66}; memcpy(er+p,mac,6); p+=6;
    er[p++]=0x01; er[p++]=0x01; er[p++]=0x00; er[p++]=0x7f; er[p++]=0xC3; /* pphy,sphy,sid,txp,rssi */
    er[p++]=0; er[p++]=0;                                              /* periodic interval */
    er[p++]=0; uint8_t z6[6]={0}; memcpy(er+p,z6,6); p+=6;             /* direct addr */
    uint8_t ad[3]={0x02,0x01,0x06}; er[p++]=3; memcpy(er+p,ad,3); p+=3; /* data_len + data */
    er[2]=p-3;
    mux_on_event(&app, er, p);
    CHECK(cont_n==1 && cont_buf[3]==0x02, "delivered as legacy report (subevent 0x02)");
    CHECK(cont_buf[5]==0x00, "event_type mapped to ADV_IND (0x00)");
    CHECK(cont_buf[6]==0x00 && memcmp(cont_buf+7,mac,6)==0, "address preserved");
    CHECK(cont_buf[13]==3 && memcmp(cont_buf+14,ad,3)==0, "adv data preserved");
    CHECK(cont_buf[17]==0xC3, "rssi preserved");

    printf("== vendor APCF filter hiding (0xFD53) ==\n");
    setup();
    uint8_t vc[]={0x01,0x53,0xfd,0x00};
    mux_submit(&app, ORIG_CONTAINER, vc, sizeof vc);
    uint8_t ccv[]={0x04,0x0e,0x0e,0x01,0x53,0xfd, 0x00,0x10,0x00,0x00,0x00,0x08,0x01,0x08,0x00,0x00};
    mux_on_event(&app, ccv, sizeof ccv);
    CHECK(cont_n==1, "vendor-cap CC delivered to container");
    CHECK(cont_buf[12]==0 && cont_buf[13]==0, "filtering_support + max_filter zeroed");
    CHECK(cont_buf[7]==0x10, "max_advt_instances preserved");

    printf("== ext-create-conn -> legacy quirk ==\n");
    setup(); app.quirk_ext_conn_to_legacy = TRUE;
    uint8_t ec[64]; p=0;
    ec[p++]=0x01; ec[p++]=0x43; ec[p++]=0x20; ec[p++]=0;
    ec[p++]=0x00; ec[p++]=0x00; ec[p++]=0x01;                         /* ifp, own, peer_addr_type */
    uint8_t pa[6]={0xde,0xad,0xbe,0xef,0x00,0x01}; memcpy(ec+p,pa,6); p+=6;
    ec[p++]=0x01;                                                     /* initiating_phys = 1M */
    uint8_t blk[16]={0x10,0,0x10,0,0x18,0,0x28,0,0,0,0x90,0x01,0,0,0,0}; memcpy(ec+p,blk,16); p+=16;
    ec[3]=p-4;
    mux_submit(&app, ORIG_CONTAINER, ec, p);
    CHECK(ctrl_n==1 && op_of(ctrl_buf)==0x200d, "0x2043 converted to 0x200d on the controller");
    CHECK(ctrl_buf[4]==0x10 && ctrl_buf[6]==0x10 && ctrl_buf[8]==0x00 && ctrl_buf[9]==0x01 &&
          memcmp(ctrl_buf+10,pa,6)==0 && ctrl_buf[16]==0x00, "legacy connection params mapped");
    uint8_t cs[]={0x04,0x0f,0x04,0x00,0x01,0x0d,0x20};               /* command status for 0x200d */
    mux_on_event(&app, cs, sizeof cs);
    CHECK(cont_n==1 && (cont_buf[5]|(cont_buf[6]<<8))==0x2043, "completion opcode rewritten back to 0x2043");

    printf("== connection ownership routing ==\n");
    setup();
    uint8_t adv[]={0x01,0x0a,0x20,0x01,0x01}; mux_submit(&app,ORIG_CONTAINER,adv,sizeof adv);
    feed_cc(0x200a); reset_caps();
    uint8_t cc[]={0x04,0x3e,0x13,0x01,0x00,0x40,0x00,0x01, 0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    mux_on_event(&app, cc, sizeof cc);
    CHECK(cont_n==1 && bz_n==0, "peripheral connection -> container only (BlueZ never sees it)");
    reset_caps();
    uint8_t acl[]={0x02,0x40,0x00,0x04,0x00,0x11,0x22,0x33,0x44};
    mux_on_event(&app, acl, sizeof acl);
    CHECK(cont_n==1 && bz_n==0, "ACL on the handle routed to the owner");
    reset_caps();
    uint8_t dis[]={0x04,0x05,0x04,0x00,0x40,0x00,0x16};
    mux_on_event(&app, dis, sizeof dis);
    CHECK(cont_n==1 && bz_n==0, "disconnection routed to owner");

    printf("== command-complete demux + credit flow ==\n");
    setup();
    uint8_t rst[]={0x01,0x03,0x0c,0x00}; mux_submit(&app, ORIG_CONTAINER, rst, sizeof rst);
    uint8_t rlv[]={0x01,0x01,0x10,0x00}; mux_submit(&app, ORIG_BLUEZ, rlv, sizeof rlv);
    CHECK(ctrl_n==1 && op_of(ctrl_buf)==0x0c03, "credit=1: first sent, second queued");
    reset_caps();
    feed_cc(0x0c03);
    CHECK(cont_n==1, "reset CC routed to its issuer (container)");
    CHECK(ctrl_n==1 && op_of(ctrl_buf)==0x1001, "freed credit flushes the queued BlueZ command");
    reset_caps();
    uint8_t cc01[]={0x04,0x0e,0x0c,0x01,0x01,0x10,0x00,0,0,0,0,0,0,0,0};
    mux_on_event(&app, cc01, sizeof cc01);
    CHECK(bz_n==1 && cont_n==0, "version CC routed to its issuer (BlueZ)");

    printf("\n%d checks, %d failures\n", tests, fails);
    return fails ? 1 : 0;
}
