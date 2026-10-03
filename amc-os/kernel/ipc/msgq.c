/* ============================================================
 * AMC OS — IPC: Mesaj Kuyrukları + Senkronize RPC (Mach tarzı port)
 * kernel/ipc/msgq.c
 *
 * Mikrokernele kalbi: sürücüler yarı-kernel modül, servisler
 * (audiod, composd, netd, sessiond) kullanıcı alanında; hepsi
 * bu kanal üzerinden konuşur. Çekirdek sadece yönlendirme yapar.
 * ============================================================ */

#include "msgq.h"
#include "../kernel.h"

struct ipc_port {
    uint32_t        port_id;       /* benzersiz port numarası     */
    struct list_head msg_queue;    /* bekleyen mesajlar           */
    uint32_t        qlimit;        /* kuyruk kapasitesi (DoS kor.)*/
    wait_queue_t    readers;       /* msgrcv'de uyuyan süreçler   */
    spinlock_t      lock;
    pid_t           owner;
};

static struct ipc_port ports[MAX_PORTS];
static id_allocator port_ids;

/* ================== Kurulum ================== */

void ipc_init(void) {
    id_alloc_init(&port_ids, 1, MAX_PORTS);
    for (int i = 0; i < MAX_PORTS; i++)
        INIT_PORT(&ports[i]);
}

uint32_t ipc_create_port(pid_t owner, uint32_t qlimit) {
    uint32_t id = id_alloc(&port_ids);
    struct ipc_port *p = &ports[id];
    p->port_id = id; p->owner = owner; p->qlimit = qlimit;
    INIT_LIST(&p->msg_queue);
    WQ_INIT(&p->readers);
    return id;
}

/* ================== Gönderim (copy-in ile güvenli) ================== */

int ipc_send(uint32_t dst_port, int from_svc, uint32_t code,
             const void __user *payload, size_t len, uint64_t aux) {
    struct ipc_port *p = &ports[dst_port];
    if (!p->port_id) return -EINVAL;
    if (len > IPC_MAX_PAYLOAD) return -EMSGSIZE;

    struct ipc_msg *m = kmalloc(sizeof(*m) + len, GFP_ATOMIC);
    if (!m) return -ENOMEM;

    /* Kullanıcı alanından çekirdeğe doğrulanmış kopya (SMAP açık!) */
    if (copy_from_user(m->data, payload, len)) {
        kfree(m);
        return -EFAULT;
    }
    m->code = code; m->sender = from_svc; m->aux = aux; m->len = len;

    scoped_spinlock(&p->lock);
    if (list_count(&p->msg_queue) >= p->qlimit) {
        kfree(m);
        return -EAGAIN;            /* dolu: gönderen bloklayabilir */
    }
    list_add_tail(&m->node, &p->msg_queue);
    wake_up_one(&p->readers);      /* bir okuyucu uyandır */
    return 0;
}

/* ================== Alım (bloklanabilir) ================== */

ssize_t ipc_recv(uint32_t port, struct ipc_msg_hdr *hdr,
                 void __user *buf, size_t bufsize, int flags) {
    struct ipc_port *p = &ports[port];
    struct ipc_msg *m;

    /* Kuyruk boşsa uyu (IPC_NOWAIT ise hemen dön) */
    while (!(m = list_first_entry_or_null(&p->msg_queue, ...))) {
        if (flags & IPC_NOWAIT) return -EAGAIN;
        interruptible_sleep_on(&p->readers);  /* sinyal gelince kalkar */
        if (signal_pending(current)) return -EINTR;
    }

    ssize_t n = MIN(m->len, bufsize);
    if (copy_to_user(buf, m->data, n)) { /* hata: mesajı geri koy */
        return -EFAULT;
    }
    hdr->code   = m->code;
    hdr->sender = m->sender;
    hdr->aux    = m->aux;
    hdr->len    = n;

    list_del(&m->node);
    kfree(m);
    return n;
}

/* ================== Senkronize RPC (istemci↔servis) ================== */

/* Örnek: masaüstü uygulamasi audiod'a "ses seviyesini ayarla" der. */
int ipc_call_rpc(uint32_t svc_port, uint32_t req_code,
                 const void *req, size_t reqlen,
                 void *resp, size_t resplen, uint64_t timeout_ms) {
    uint32_t reply_port = ipc_create_port(current->pid, 4);
    struct rpc_frame f = { .reply_port = reply_port, .seq = next_seq() };

    /* istek: [frame başlığı][payload] */
    ipc_send(svc_port, current->svc_id, req_code, req, reqlen,
             (uint64_t)&f | RPC_FLAG_WANT_REPLY);

    /* yanıt bekle (timeout ile) */
    struct ipc_msg_hdr hdr;
    ssize_t r = ipc_recv_timeout(reply_port, &hdr, resp, resplen, timeout_ms);
    ipc_destroy_port(reply_port);
    return r < 0 ? (int)r : 0;
}

/* ================== Servis kayıt defteri ================== */

static struct { const char *name; uint32_t port; } services[] = {
    { "audiod",     SVC_AUDIOD     },  /* ses servisi (HDA arayüzü)   */
    { "composd",    SVC_COMPOSITOR },  /* compositor / masaüstü       */
    { "netd",       SVC_NETD       },  /* ağ yöneticisi (dhcp/dns)    */
    { "sessiond",   SVC_SESSIOND   },  /* oturum/girdi yönetimi       */
    { "pkgd",       SVC_PKGD       },  /* paket/port yöneticisi       */
    { "powerd",     SVC_POWERD     },  /* güç/USB port yönetim        */
};

uint32_t ipc_lookup_service(const char *name) {
    for (auto &s : services)
        if (!strcmp(s.name, name)) return s.port;
    return PORT_NONE;
}

/* ================== İzleme (geliştirici için) ================== */

/* `amctrace ipc` komutu bu iç halkayı okur: her mesajın süresi,
   port kuyruk doluluğu → performans profiler'ına ham veri */
struct ipc_trace_ring trace_ring;

void ipc_trace_record(uint32_t src, uint32_t dst, uint32_t code,
                      uint64_t latency_ns) {
    trace_ring.buf[trace_ring.head++ % TRACE_SIZE] =
        (struct ipc_trace){ src, dst, code, latency_ns, ktime_ns() };
}
