/* ============================================================
 * AMC OS — AMCFS: Günlüklü (journaling) dosya sistemi
 * fs/amcfs.c
 *   - 128-byte inode, extent tabanlı blok haritalama (SSD dostu)
 *   - Write-ahead journal: çökme sonrası veri kaybı YOK (ext3/4 gibi)
 *   - Hard link, symlink, dizin ağacı, dosya izinleri (POSIX mode)
 *   - Checksum'lı meta veriler (bit-rot algılama)
 *   - USB bellek / SD kart otomatik mount politikası (powerd ile)
 * ============================================================ */

#include "amcfs.h"
#include "../kernel.h"

#define AMCFS_MAGIC     0x414D4346   /* "AMCF" */
#define BLOCK_SIZE      4096
#define JOURNAL_BLOCKS  256           /* 1MB günlük */

struct amc_superblock {
    uint32_t magic;
    uint32_t version;
    uint64_t total_blocks, free_blocks;
    uint64_t inode_count, next_inode;
    uint64_t journal_start, journal_len;
    uint32_t block_size;
    uint32_t checksum;                /* CRC32 of header            */
    char     label[32];
};

struct amc_inode {
    uint16_t mode;                    /* rwx + tip (dosya/dizin/link)*/
    uint16_t uid, gid;
    uint32_t atime, mtime, ctime;
    uint32_t size_lo, size_hi;        /* 48-bit dosya boyutu         */
    uint32_t flags;                   /* immutable, append-only...   */
    struct amc_extent extents[12];    /* extent: start_block, length */
    uint32_t xattr_block;             /* genişletilmiş öznitelikler  */
    uint32_t checksum;
};

struct amc_extent {
    uint64_t block_start;
    uint32_t block_count;             /* bit 31 = hala-yazılıyor     */
    uint32_t reserved;
};

/* ================== Mount + günlük kurtarma ================== */

int amcfs_mount_root(void) {
    struct block_dev *dev = find_boot_device();   /* /dev/sda2 vb.   */
    if (!dev) return -ENODEV;

    struct amc_superblock sb;
    bread(dev, 0, &sb);
    if (sb.magic != AMCFS_MAGIC || crc32(&sb) != sb.checksum)
        return -EBADMSG;

    /* Journal replay: commit edilmemiş blokları geri al,
       tamamlanmış transaction'ları diske uygula */
    int recovered = journal_replay(dev, &sb);
    if (recovered > 0)
        kprintf("[amcfs] gunlukten %d islem geri yuklendi (crash recovery)\n",
                recovered);

    fs_register(mkfs_instance(dev, &sb));
    return 0;
}

/* ================== Günlükleme (WAL) yazma yolu ================== */

/* Her değişiklik önce günlüğe, sonra hedef bloğa, en son günlük
   checkpoint'i yazılır → güç kesilirse tutarlı eski durum kalır. */
static int journal_write_tx(struct amc_fs *fs, struct jnode *ops, int n) {
    scoped_spinlock(&fs->journal_lock);
    uint32_t txid = ++fs->tx_counter;

    for (int i = 0; i < n; i++)
        journal_append(fs, txid, ops[i].block, ops[i].data);
    journal_commit_record(fs, txid);          /* COMMIT imzası */
    disk_flush(fs->dev);                      /* FUA/barrier  */

    for (int i = 0; i < n; i++)               /* hedefe uygula */
        bwrite(fs->dev, ops[i].block, ops[i].data);
    disk_flush(fs->dev);

    journal_checkpoint(fs, txid);             /* boş alan ilerlet */
    return 0;
}

/* ================== Dosya işlemleri ================== */

int amcfs_open(struct vnode *vn, int flags) {
    struct amc_inode ino;
    inode_read(vn->fs, vn->ino_number, &ino);

    if ((flags & O_WRONLY || flags & O_RDWR) && !check_perm(&ino, W))
        return -EACCES;
    if ((flags & O_TRUNC) && check_perm(&ino, W))
        truncate_to(vn, 0);                   /* extents serbest bırak */
    if ((flags & O_CREAT) && !exists(vn))
        return inode_create_parent_dir(vn, S_IFREG | 0644);
    return 0;
}

ssize_t amcfs_read(struct vnode *vn, void *buf, size_t len, loff_t off) {
    /* Extent içinde offset→blok hesabı: O(1), fragment yok */
    struct amc_extent *e = extent_for_offset(vn->fs, &vn->ino, off);
    if (!e) return 0;                         /* sparse delik: sıfır döner */
    return bio_read_spans(vn->fs->dev, e, off, buf, len);
}

ssize_t amcfs_write(struct vnode *vn, const void *buf, size_t len, loff_t off) {
    if (vn->ino.flags & INODE_IMMUTABLE) return -EPERM;

    struct jnode ops[8]; int n = 0;
    /* Yeterli yer yoksa extent büyüt (COW değil: doğrudan WAL) */
    if (off + len > vn->ino.size)
        n += extent_allocate(vn->fs, &vn->ino, off+len, ops);

    n += copy_dirty_buffers(vn, buf, len, off, ops);
    vn->ino.mtime = ktime_seconds();
    inode_serialize(&vn->ino, ops[n++]);      /* inode bloğu da günlüğe */

    return journal_write_tx(vn->fs, ops, n) < 0 ? -EIO : (ssize_t)len;
}

/* ================== Dizin tarama ================== */

int amcfs_readdir(struct vnode *dir, struct dir_entry *out, int max) {
    /* Basit lineer dizin bloğu listesi; büyük dizinlerde B+ ağacı
       planlandı (v0.5). Her girdi: ino + ad + tip baytı */
    return dir_iter_blocks(dir, out, max);
}

/* ================== USB/SD otomatik mount (powerd servisi) ================== */

void amcfs_automount_hook(struct block_dev *new_disk) {
    /* Takılan her yeni disk bölümlenir; bilinen FS'ler /media altına
       etiketleriyle bağlanır → masaüstünde simge olarak belirir */
    partition_scan(new_disk);
    for_each_partition(p, new_disk) {
        char path[64];
        snprintf(path, sizeof(path), "/media/%s", p->label ?: p->uuid);
        fs_mount_guess_type(p, path);         /* AMCF/ext4/FAT/NTFS-ro */
        ipc_post_msg(SVC_SESSIOND, DESKTOP_NEW_ICON, p->id, 0);
    }
}
