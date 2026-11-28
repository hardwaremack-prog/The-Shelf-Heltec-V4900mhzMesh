# SD Card Imager - save a whole SD card to an image file, and write an image back onto a card.
# Windows 10/11, nothing to install. Start it with "SD Card Imager.cmd" (it asks for admin rights,
# which Windows needs for reading or writing a whole card).
#
# Only SD cards and USB drives are listed (never the computer's own disks). Images are saved as
# .img (exact copy) or .img.gz (compressed: empty space shrinks to almost nothing).
# "Grow to fill card" makes the last partition use the whole card after a smaller card's image
# was written onto a bigger one (FAT32 and NTFS fully; a Pi's ext4 partition then needs one
# command on the Pi).

# ---- run as administrator (reading a whole disk needs it)
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$PSCommandPath`""
    exit
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

try {
Add-Type -ErrorAction Stop -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class RawDisk {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sec, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr inBuf, uint inSize, IntPtr outBuf, uint outSize, out uint returned, IntPtr overlapped);

    const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000, SHARE_READ_WRITE = 3, OPEN_EXISTING = 3;
    const uint FSCTL_LOCK_VOLUME = 0x00090018, FSCTL_DISMOUNT_VOLUME = 0x00090020;

    public static FileStream OpenDisk(int number, bool write) {
        SafeFileHandle h = CreateFile(@"\\.\PhysicalDrive" + number, write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
                                      SHARE_READ_WRITE, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (h.IsInvalid) throw new IOException("Windows wouldn't open the card (error " + Marshal.GetLastWin32Error() + ").");
        return new FileStream(h, write ? FileAccess.ReadWrite : FileAccess.Read, 4096, false);
    }

    // Takes a volume on the card away from Windows while it's being written (so nothing else
    // writes to it at the same time). Keep the handle until the writing is done.
    public static SafeFileHandle LockVolume(string path) {
        SafeFileHandle h = CreateFile(path, GENERIC_READ | GENERIC_WRITE, SHARE_READ_WRITE, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (h.IsInvalid) throw new IOException("Windows wouldn't let go of the card (error " + Marshal.GetLastWin32Error() + ").");
        uint r;
        if (!DeviceIoControl(h, FSCTL_LOCK_VOLUME, IntPtr.Zero, 0, IntPtr.Zero, 0, out r, IntPtr.Zero)) {
            h.Dispose();
            throw new IOException("The card is in use. Close any windows or programs showing files on it, then try again.");
        }
        DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, IntPtr.Zero, 0, IntPtr.Zero, 0, out r, IntPtr.Zero);
        return h;
    }

    // An uncompressed image file that skips empty space on disk (NTFS sparse file)
    public static void SetSparse(FileStream f) {
        uint r;
        DeviceIoControl(f.SafeFileHandle, 0x000900C4, IntPtr.Zero, 0, IntPtr.Zero, 0, out r, IntPtr.Zero);   // FSCTL_SET_SPARSE
    }
}

// Which parts of a FAT32 card hold anything. Everything up to the end of the FAT tables is
// always copied; after that, only clusters the FAT marks as used. Empty clusters are left out
// (zeros in the image, skipped on the card). Anything that isn't plain FAT32 is copied whole.
public class FatMap {
    public long PartStart, PartEnd, FatStart, DataStart;
    public int ClusterSize;
    public long ClusterCount;
    public bool OtherPartitions;
    uint[] fat;

    static uint U32(byte[] b, long o) { return (uint)(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)); }
    static int U16(byte[] b, long o) { return b[o] | (b[o + 1] << 8); }
    static bool IsFat32Boot(byte[] b, long o, long len) {
        if (o + 512 > len) return false;
        if (b[o + 510] != 0x55 || b[o + 511] != 0xAA) return false;
        return b[o + 82] == (byte)'F' && b[o + 83] == (byte)'A' && b[o + 84] == (byte)'T' && b[o + 85] == (byte)'3' && b[o + 86] == (byte)'2';
    }

    // How many bytes from the start of the card are needed to read the map: -1 = not FAT32 (copy
    // everything); a number bigger than len = read that much first; otherwise the map is ready.
    public static long BytesNeeded(byte[] b, long len, out long part, out bool others) {
        part = -1; others = false;
        if (len < 512) return 512;
        if (b[510] != 0x55 || b[511] != 0xAA) return -1;
        if (IsFat32Boot(b, 0, len)) part = 0;                        // no partition table
        else {
            for (int i = 0; i < 4; i++) {
                long e = 446 + 16 * i;
                byte type = b[e + 4];
                if (type == 0) continue;
                if (part < 0 && (type == 0x0B || type == 0x0C)) part = (long)U32(b, e + 8) * 512;
                else others = true;
            }
            if (part < 0) return -1;
        }
        if (len < part + 512) return part + 512;
        if (!IsFat32Boot(b, part, len)) return -1;
        int bps = U16(b, part + 11), spc = b[part + 13], rsvd = U16(b, part + 14), nf = b[part + 16];
        uint fatsz = U32(b, part + 36);
        if (bps < 512 || bps > 4096 || spc == 0 || nf == 0 || fatsz == 0) return -1;
        return part + ((long)rsvd + (long)nf * fatsz) * bps;
    }

    public static FatMap Build(byte[] b, long len) {
        long part; bool others;
        long need = BytesNeeded(b, len, out part, out others);
        if (need < 0 || need > len) return null;
        FatMap m = new FatMap();
        int bps = U16(b, part + 11), spc = b[part + 13], rsvd = U16(b, part + 14), nf = b[part + 16];
        uint fatsz = U32(b, part + 36), tot = U32(b, part + 32);
        m.PartStart = part; m.OtherPartitions = others;
        m.PartEnd = part + (long)tot * bps;
        m.FatStart = part + (long)rsvd * bps;
        m.DataStart = need;
        m.ClusterSize = bps * spc;
        m.ClusterCount = ((long)tot - rsvd - (long)nf * fatsz) / spc;
        long entries = Math.Min((long)fatsz * bps / 4, m.ClusterCount + 2);
        m.fat = new uint[entries];
        for (long i = 0; i < entries; i++) m.fat[i] = U32(b, m.FatStart + i * 4) & 0x0FFFFFFF;
        return m;
    }

    // Reads the map straight from a card (from its first bytes); null if it isn't FAT32.
    public static FatMap Probe(Stream s, long size) {
        byte[] b = new byte[1 << 20]; long len = 0;
        for (;;) {
            long part; bool others;
            long need = BytesNeeded(b, len, out part, out others);
            if (need < 0 || need > size || need > (1L << 30)) return null;
            if (need <= len) return Build(b, len);
            long want = (need + 4095) & ~4095L;
            if (want > b.Length) { byte[] nb = new byte[Math.Max(want, (long)b.Length * 2)]; Array.Copy(b, nb, len); b = nb; }
            s.Seek(len, SeekOrigin.Begin);
            while (len < want) { int n = s.Read(b, (int)len, (int)(want - len)); if (n <= 0) return null; len += n; }
        }
    }

    public long UsedBytes() {
        long used = 0;
        for (long c = 2; c < fat.Length; c++) if (fat[c] != 0) used += ClusterSize;
        return used + DataStart;
    }

    // Does this stretch of the card hold anything?
    public bool Needed(long off, long len) {
        long end = off + len;
        if (off < DataStart) return true;
        if (off >= PartEnd) return OtherPartitions;                  // past the partition
        if (end > PartEnd && OtherPartitions) return true;
        long last = Math.Min(end, PartEnd) - 1;
        long c0 = 2 + (off - DataStart) / ClusterSize, c1 = 2 + (last - DataStart) / ClusterSize;
        for (long c = c0; c <= c1; c++) {
            if (c >= fat.Length) break;                              // slack after the last cluster
            if (fat[c] != 0) return true;
        }
        return false;
    }
}

// The start of an image as it streams past, until there's enough to read its map.
public class HeadCollector {
    MemoryStream ms = new MemoryStream();
    public FatMap Map;
    public bool Done;
    public void Add(byte[] b, int n) {
        if (Done) return;
        ms.Write(b, 0, n);
        byte[] buf = ms.GetBuffer(); long len = ms.Length;
        long part; bool others;
        long need = FatMap.BytesNeeded(buf, len, out part, out others);
        if (need < 0 || len > (1L << 30)) { Done = true; ms = null; return; }
        if (need <= len) { Map = FatMap.Build(buf, len); Done = true; ms = null; }
    }
}
// Makes the last partition on a card fill the whole card, after a smaller card's image was
// written onto a bigger one. (Plain C# 5 so Windows PowerShell 5.1 can compile it.)
//   FAT32 : the partition and the file system both grow here (the FAT tables get bigger; the
//           first few clusters move out of their way, and every reference to them is updated).
//   Linux : the partition grows here; the ext4 file system inside then grows on the Pi with one
//           command (resize2fs), because Windows can't change ext4.
//   NTFS  : the PowerShell side uses Windows' own Resize-Partition.
public class GrowPlan {
    public string Kind = "none";        // fat32, linux, ntfs, full, unsupported
    public string Message = "";
    public int Entry = -1;              // MBR entry 0-3, or -1 for a card with no partition table
    public int PartNumber;              // Windows' partition number (1-based)
    public long Start, OldSectors, NewSectors, DiskSectors;
    public bool CanGrow { get { return Kind == "fat32" || Kind == "linux" || Kind == "ntfs"; } }
}

public static class CardGrower {
    const int SECTOR = 512;
    const uint EOC_MIN = 0x0FFFFFF8, BAD = 0x0FFFFFF7;

    static uint U32(byte[] b, int o) { return (uint)(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)); }
    static int U16(byte[] b, int o) { return b[o] | (b[o + 1] << 8); }
    static void P32(byte[] b, int o, uint v) { b[o] = (byte)v; b[o + 1] = (byte)(v >> 8); b[o + 2] = (byte)(v >> 16); b[o + 3] = (byte)(v >> 24); }

    static void ReadAt(Stream s, long off, byte[] buf, int len) {
        s.Seek(off, SeekOrigin.Begin);
        int got = 0;
        while (got < len) { int n = s.Read(buf, got, len - got); if (n <= 0) throw new IOException("The card stopped answering while being read."); got += n; }
    }
    static byte[] ReadAt(Stream s, long off, int len) { byte[] b = new byte[len]; ReadAt(s, off, b, len); return b; }
    static void WriteAt(Stream s, long off, byte[] buf, int len) { s.Seek(off, SeekOrigin.Begin); s.Write(buf, 0, len); s.Flush(); }

    static bool IsFat32Boot(byte[] b) {
        return b[510] == 0x55 && b[511] == 0xAA && b[82] == 'F' && b[83] == 'A' && b[84] == 'T' && b[85] == '3' && b[86] == '2';
    }
    static string Oem(byte[] b) { return System.Text.Encoding.ASCII.GetString(b, 3, 8); }

    // Looks at the card and works out what growing it would do. Changes nothing.
    public static GrowPlan Check(Stream s, long diskBytes) {
        GrowPlan p = new GrowPlan();
        p.DiskSectors = Math.Min(diskBytes / SECTOR, 0xFFFFFFFFL);            // MBR limit (2 TB)
        byte[] mbr = ReadAt(s, 0, SECTOR);
        if (mbr[510] != 0x55 || mbr[511] != 0xAA) { p.Kind = "unsupported"; p.Message = "The card has no partition table that this app understands."; return p; }

        if (IsFat32Boot(mbr)) {                                                 // whole card is one FAT32 volume
            p.Entry = -1; p.PartNumber = 0; p.Start = 0; p.OldSectors = U32(mbr, 32); p.Kind = "fat32";
        } else {
            long bestEnd = -1; int count = 0;
            for (int i = 0; i < 4; i++) {
                int e = 446 + 16 * i; byte type = mbr[e + 4];
                if (type == 0) continue;
                count++;
                if (type == 0xEE) { p.Kind = "unsupported"; p.Message = "This card uses a GPT partition table, which this app doesn't grow yet."; return p; }
                if (type == 0x05 || type == 0x0F || type == 0x85) { p.Kind = "unsupported"; p.Message = "This card has an extended partition, which this app doesn't grow."; return p; }
                long st = U32(mbr, e + 8), sz = U32(mbr, e + 12);
                if (st + sz > bestEnd) { bestEnd = st + sz; p.Entry = i; p.Start = st; p.OldSectors = sz; p.PartNumber = count; }
            }
            if (p.Entry < 0) { p.Kind = "unsupported"; p.Message = "The card has no partitions."; return p; }
            // Windows numbers partitions in table order; count entries up to the one we picked
            int num = 0; for (int i = 0; i <= p.Entry; i++) if (mbr[446 + 16 * i + 4] != 0) num++;
            p.PartNumber = num;
            byte t = mbr[446 + 16 * p.Entry + 4];
            byte[] boot = ReadAt(s, p.Start * SECTOR, SECTOR);
            if ((t == 0x0B || t == 0x0C) && IsFat32Boot(boot)) p.Kind = "fat32";
            else if (t == 0x83) {
                byte[] sb = ReadAt(s, p.Start * SECTOR + 1024, SECTOR);
                p.Kind = (sb[56] == 0x53 && sb[57] == 0xEF) ? "linux" : "unsupported";
                if (p.Kind == "unsupported") p.Message = "The last partition is a Linux partition but not ext4, so it's left alone.";
            }
            else if (t == 0x07 && Oem(boot) == "NTFS    ") p.Kind = "ntfs";
            else if (t == 0x07 && Oem(boot) == "EXFAT   ") { p.Kind = "unsupported"; p.Message = "The last partition is exFAT, which this app can't grow."; }
            else { p.Kind = "unsupported"; p.Message = "The last partition (type " + t.ToString("X2") + ") isn't one this app knows how to grow."; }
            if (p.Kind == "unsupported") return p;
        }

        long newSectors = p.DiskSectors - p.Start;
        newSectors -= newSectors % 2048;                                        // end on a 1 MB boundary
        if (newSectors - p.OldSectors < 2048 * 64) {                            // less than 64 MB to gain
            p.Kind = "full"; p.Message = "The card is already using all of its space."; return p;
        }
        p.NewSectors = newSectors;
        if (p.Kind == "fat32") {
            byte[] boot = ReadAt(s, p.Start * SECTOR, SECTOR);
            if (U16(boot, 11) != SECTOR) { p.Kind = "unsupported"; p.Message = "This FAT32 volume uses " + U16(boot, 11) + "-byte sectors, which this app doesn't grow."; return p; }
        }
        p.Message = "Grows from " + (p.OldSectors * SECTOR / 1000000000.0).ToString("0.0") + " GB to " + (p.NewSectors * SECTOR / 1000000000.0).ToString("0.0") + " GB.";
        return p;
    }

    // Writes the bigger size into the partition table entry.
    static void SetPartitionSize(Stream s, GrowPlan p) {
        if (p.Entry < 0) return;
        byte[] mbr = ReadAt(s, 0, SECTOR);
        int e = 446 + 16 * p.Entry;
        P32(mbr, e + 12, (uint)p.NewSectors);
        mbr[e + 5] = 0xFE; mbr[e + 6] = 0xFF; mbr[e + 7] = 0xFF;               // CHS end: "use LBA"
        WriteAt(s, 0, mbr, SECTOR);
    }

    public static void Grow(Stream s, GrowPlan p, Action<string> say) {
        if (p.Kind == "linux") { say("Making the partition bigger..."); SetPartitionSize(s, p); return; }
        if (p.Kind != "fat32") throw new InvalidOperationException("Nothing to grow here.");
        GrowFat32(s, p, say);
        SetPartitionSize(s, p);
    }

    // ---- FAT32
    static void GrowFat32(Stream s, GrowPlan p, Action<string> say) {
        long ps = p.Start * SECTOR;
        byte[] boot = ReadAt(s, ps, SECTOR);
        int spc = boot[13], rsvd = U16(boot, 14), nf = boot[16];
        long fatSz = U32(boot, 36), tot = U32(boot, 32);
        uint root = U32(boot, 44);
        int fsInfo = U16(boot, 48), bkBoot = U16(boot, 50);
        if (spc == 0 || nf == 0 || fatSz == 0) throw new IOException("The FAT32 volume's header doesn't make sense.");
        long cs = (long)spc * SECTOR;
        long fatStart = ps + (long)rsvd * SECTOR;
        long dataStart = fatStart + nf * fatSz * SECTOR;
        long oldN = (tot - rsvd - nf * fatSz) / spc;                            // clusters 2 .. oldN+1
        long newTot = p.NewSectors;

        // How much bigger the FAT tables must get: k clusters' worth of room after them.
        long k = 0, newFat = fatSz, newN = 0;
        for (; ; k++) {
            if ((k * spc) % nf != 0) continue;
            newFat = fatSz + k * spc / nf;
            newN = (newTot - rsvd - nf * newFat) / spc;
            if (newFat * SECTOR / 4 >= newN + 2) break;
            if (k > oldN) throw new IOException("This card's file system is too full to make room for bigger FAT tables.");
        }
        if (newN >= 0x0FFFFFF5) throw new IOException("That's more clusters than FAT32 allows.");

        say("Reading the card's file table...");
        uint[] fat = new uint[oldN + 2];
        {
            long bytes = (oldN + 2) * 4, done = 0;
            byte[] buf = new byte[4 << 20];
            while (done < bytes) {
                int want = (int)Math.Min(buf.Length, ((bytes - done) + SECTOR - 1) / SECTOR * SECTOR);
                ReadAt(s, fatStart + done, buf, want);
                for (int i = 0; i + 3 < want && done / 4 + i / 4 < fat.Length; i += 4) fat[done / 4 + i / 4] = U32(buf, i) & 0x0FFFFFFF;
                done += want;
            }
            fat[0] = U32(ReadAt(s, fatStart, SECTOR), 0); fat[1] = U32(ReadAt(s, fatStart, SECTOR), 4);
        }

        // Old clusters 2..k+1 sit where the bigger tables will go: give the used ones new homes at
        // the end of the old data. Every other cluster keeps its place but its number drops by k.
        long[] reloc = new long[k + 2];
        long next = oldN + 2 - k;                                               // first new-numbered cluster past the old data
        List<long> moving = new List<long>();
        for (long c = 2; c <= k + 1 && c < oldN + 2; c++) {
            if (fat[c] == 0 || fat[c] == BAD) continue;
            reloc[c] = next++; moving.Add(c);
        }
        if (next - 1 > newN + 1) throw new IOException("Not enough room to move the first clusters.");
        Func<long, long> map = delegate (long c) { return c <= k + 1 ? reloc[c] : c - k; };
        Func<long, long> oldPhys = delegate (long c) { return dataStart + (c - 2) * cs; };
        long newDataStart = dataStart + k * cs;
        Func<long, long> newPhys = delegate (long n) { return newDataStart + (n - 2) * cs; };

        // Check every chain and directory before changing anything.
        Func<uint, bool> okLink = delegate (uint v) { return v >= EOC_MIN || v == BAD || (v >= 2 && v < oldN + 2); };
        for (long c = 2; c < oldN + 2; c++) {
            if (fat[c] == 0) continue;
            if (!okLink(fat[c])) throw new IOException("The card's file table has a broken chain at cluster " + c + ". Run a disk check on it first.");
            uint v0 = fat[c];
            if (v0 < EOC_MIN && v0 != BAD && v0 <= k + 1 && reloc[v0] == 0) throw new IOException("The card's file table points at an empty cluster (" + v0 + "). Run a disk check on it first.");
        }
        say("Reading the folders...");
        List<List<long>> dirs = new List<List<long>>();                         // each folder's clusters, in order
        Dictionary<long, bool> seenDir = new Dictionary<long, bool>();
        Queue<long> todo = new Queue<long>();
        todo.Enqueue(root);
        while (todo.Count > 0) {
            long first = todo.Dequeue();
            if (first < 2 || first >= oldN + 2 || seenDir.ContainsKey(first)) continue;
            seenDir[first] = true;
            List<long> chain = new List<long>();
            Dictionary<long, bool> inChain = new Dictionary<long, bool>();
            for (long c = first; c >= 2 && c < oldN + 2 && !inChain.ContainsKey(c); ) {
                inChain[c] = true; chain.Add(c);
                uint v = fat[c];
                if (v == 0 || v >= EOC_MIN || v == BAD) break;
                c = v;
            }
            dirs.Add(chain);
            bool ended = false;
            foreach (long c in chain) {
                if (ended) break;
                byte[] d = ReadAt(s, oldPhys(c), (int)cs);
                for (int o = 0; o < cs; o += 32) {
                    if (d[o] == 0x00) { ended = true; break; }
                    if (d[o] == 0xE5 || d[o + 11] == 0x0F) continue;
                    long cl = (long)(((uint)U16(d, o + 20) << 16) | (uint)U16(d, o + 26));
                    if (cl == 0) continue;
                    if (cl < 2 || cl >= oldN + 2 || (cl <= k + 1 && reloc[cl] == 0)) throw new IOException("A folder entry points outside the card (cluster " + cl + "). Run a disk check on it first.");
                    bool dot = d[o] == '.';
                    if ((d[o + 11] & 0x10) != 0 && (d[o + 11] & 0x08) == 0 && !dot) todo.Enqueue(cl);
                }
            }
        }

        // 1. copy the clusters that have to move (into space nothing uses yet)
        say("Moving " + moving.Count + " clusters out of the way...");
        byte[] cbuf = new byte[cs];
        foreach (long c in moving) { ReadAt(s, oldPhys(c), cbuf, (int)cs); WriteAt(s, newPhys(reloc[c]), cbuf, (int)cs); }

        // 2. update the cluster numbers in every folder
        say("Updating " + dirs.Count + " folders...");
        foreach (List<long> chain in dirs) {
            bool ended = false;
            foreach (long c in chain) {
                if (ended) break;
                long where = newPhys(map(c));
                byte[] d = ReadAt(s, where, (int)cs);
                bool changed = false;
                for (int o = 0; o < cs; o += 32) {
                    if (d[o] == 0x00) { ended = true; break; }
                    if (d[o] == 0xE5 || d[o + 11] == 0x0F) continue;
                    long cl = (long)(((uint)U16(d, o + 20) << 16) | (uint)U16(d, o + 26));
                    if (cl == 0) continue;
                    long n = map(cl);
                    if (n == cl) continue;
                    d[o + 20] = (byte)(n >> 16); d[o + 21] = (byte)(n >> 24);
                    d[o + 26] = (byte)n; d[o + 27] = (byte)(n >> 8);
                    changed = true;
                }
                if (changed) WriteAt(s, where, d, (int)cs);
            }
        }

        // 3. write the bigger FAT tables
        say("Writing the bigger file tables...");
        long tableBytes = newFat * SECTOR;
        uint[] nfat = new uint[newN + 2];
        nfat[0] = fat[0]; nfat[1] = fat[1];
        long used = 0;
        for (long c = 2; c < oldN + 2; c++) {
            uint v = fat[c];
            if (v == 0) continue;
            if (v == BAD && c <= k + 1) continue;                               // bad spot now under the tables
            uint nv = (v >= EOC_MIN || v == BAD) ? v : (uint)map(v);
            nfat[map(c)] = nv; used++;
        }
        byte[] tbuf = new byte[4 << 20];
        for (int copy = 0; copy < nf; copy++) {
            long baseOff = fatStart + copy * tableBytes;
            for (long done = 0; done < tableBytes; done += tbuf.Length) {
                int len = (int)Math.Min(tbuf.Length, tableBytes - done);
                Array.Clear(tbuf, 0, len);
                long firstEntry = done / 4;
                for (int i = 0; i < len / 4; i++) {
                    long ent = firstEntry + i;
                    if (ent >= nfat.Length) break;
                    P32(tbuf, i * 4, nfat[ent]);
                }
                WriteAt(s, baseOff + done, tbuf, len);
            }
        }

        // 4. the volume's header (and its backup copy) and the free-space note
        say("Updating the volume header...");
        int[] heads = (bkBoot > 0 && bkBoot < rsvd) ? new int[] { 0, bkBoot } : new int[] { 0 };
        foreach (int h in heads) {
            byte[] b = ReadAt(s, ps + (long)h * SECTOR, SECTOR);
            P32(b, 32, (uint)newTot); P32(b, 36, (uint)newFat); P32(b, 44, (uint)map(root));
            WriteAt(s, ps + (long)h * SECTOR, b, SECTOR);
            int fi = fsInfo > 0 && fsInfo < rsvd ? h + fsInfo : -1;
            if (fi > 0 && fi < rsvd) {
                byte[] f = ReadAt(s, ps + (long)fi * SECTOR, SECTOR);
                if (U32(f, 0) == 0x41615252 && U32(f, 484) == 0x61417272) {
                    P32(f, 488, (uint)(newN - used)); P32(f, 492, 0xFFFFFFFF);
                    WriteAt(s, ps + (long)fi * SECTOR, f, SECTOR);
                }
            }
        }
    }
}
'@
} catch {
    [void][System.Windows.Forms.MessageBox]::Show("SD Card Imager couldn't start: $($_.Exception.Message)", 'SD Card Imager')
    exit
}

$CHUNK = 4MB      # a multiple of every card's sector size

function Format-Size([double]$b) {
    if ($b -ge 1GB) { '{0:N1} GB' -f ($b / 1GB) } elseif ($b -ge 1MB) { '{0:N0} MB' -f ($b / 1MB) } else { '{0:N0} KB' -f ($b / 1KB) }
}
function Format-Time([double]$s) {
    if ($s -lt 60) { '{0:N0} s' -f $s } elseif ($s -lt 3600) { '{0:N0} min' -f ($s / 60) } else { '{0:N1} h' -f ($s / 3600) }
}

# The kind of connection Windows reports for a disk (it can come back as a word or a number)
function Bus-Name($d) {
    $b = "$($d.BusType)"
    switch ($b) { '7' { 'USB' } '12' { 'SD' } '13' { 'MMC' } '1' { 'SCSI' } '3' { 'ATA' } '11' { 'SATA' } '17' { 'NVMe' } '8' { 'RAID' } default { $b } }
}

# SD cards and USB drives, never the disk Windows runs from. Card readers built into a computer
# often report themselves as SCSI or ATA rather than SD, so "removable media" counts too.
# "Show all drives" lists every disk except Windows' own.
function Get-Cards {
    $removable = @{}
    foreach ($w in @(Get-CimInstance Win32_DiskDrive -ErrorAction SilentlyContinue)) {
        if ("$($w.MediaType)" -match 'Removable|External' -or "$($w.InterfaceType)" -eq 'USB' -or "$($w.PNPDeviceID)" -match 'SD|MMC|CARD|USBSTOR') { $removable[[int]$w.Index] = $true }
    }
    $list = @()
    foreach ($d in @(Get-Disk -ErrorAction SilentlyContinue)) {
        if ($d.IsBoot -or $d.IsSystem -or -not $d.Size -or $d.Size -le 0) { continue }
        $bus = Bus-Name $d
        $isCard = ($bus -in @('SD', 'USB', 'MMC')) -or $removable.ContainsKey([int]$d.Number)
        if (-not $isCard -and -not $showAll.Checked) { continue }
        if ($d.Size -gt 2TB -and -not $showAll.Checked) { continue }
        $letters = @(Get-Partition -DiskNumber $d.Number -ErrorAction SilentlyContinue | Where-Object { $_.DriveLetter } | ForEach-Object { "$($_.DriveLetter):" })
        $name = ("$($d.FriendlyName)" -replace '\s+', ' ').Trim()
        $label = "Disk $($d.Number): $name, $(Format-Size $d.Size), $bus" + $(if ($letters.Count) { "  ($($letters -join ' '))" } else { '' })
        $list += [pscustomobject]@{ Number = [int]$d.Number; Size = [int64]$d.Size; Label = $label; Letters = $letters }
    }
    $list
}

# What Windows sees, for when no card shows up
function Disk-Summary {
    $parts = foreach ($d in @(Get-Disk -ErrorAction SilentlyContinue)) {
        $tag = if ($d.IsBoot -or $d.IsSystem) { ', Windows' } else { '' }
        "Disk $($d.Number) $(Bus-Name $d) $(Format-Size $d.Size)$tag"
    }
    if ($parts) { $parts -join ' | ' } else { 'no disks at all (is this running as administrator?)' }
}

# ---- window
$form = New-Object Windows.Forms.Form
$form.Text = 'SD Card Imager'
$form.ClientSize = New-Object Drawing.Size(560, 350)
$form.AutoScaleDimensions = New-Object Drawing.SizeF(96, 96); $form.AutoScaleMode = 'Dpi'
$form.FormBorderStyle = 'FixedSingle'; $form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.Font = New-Object Drawing.Font('Segoe UI', 10)

$title = New-Object Windows.Forms.Label
$title.Text = 'Save an SD card to an image file, or write an image onto a card.'
$title.Location = New-Object Drawing.Point(16, 14); $title.AutoSize = $true
$form.Controls.Add($title)

$lbl = New-Object Windows.Forms.Label
$lbl.Text = 'Card:'; $lbl.Location = New-Object Drawing.Point(16, 52); $lbl.AutoSize = $true
$form.Controls.Add($lbl)

$combo = New-Object Windows.Forms.ComboBox
$combo.DropDownStyle = 'DropDownList'
$combo.Location = New-Object Drawing.Point(64, 48); $combo.Size = New-Object Drawing.Size(380, 28)
$form.Controls.Add($combo)

$refresh = New-Object Windows.Forms.Button
$refresh.Text = 'Refresh'; $refresh.Location = New-Object Drawing.Point(452, 47); $refresh.Size = New-Object Drawing.Size(92, 30)
$form.Controls.Add($refresh)

$compress = New-Object Windows.Forms.CheckBox
$compress.Text = 'Compress the image (.img.gz: much smaller, empty space takes almost nothing)'
$compress.Checked = $true; $compress.Location = New-Object Drawing.Point(16, 88); $compress.AutoSize = $true
$form.Controls.Add($compress)

$quick = New-Object Windows.Forms.CheckBox
$quick.Text = 'Quick: only the parts in use (FAT32 cards)'
$quick.Checked = $true; $quick.Location = New-Object Drawing.Point(16, 272); $quick.AutoSize = $true
$form.Controls.Add($quick)

$showAll = New-Object Windows.Forms.CheckBox
$showAll.Text = 'Show all drives'
$showAll.Location = New-Object Drawing.Point(16, 300); $showAll.AutoSize = $true
$form.Controls.Add($showAll)
$showAll.Add_CheckedChanged({ Load-Cards })

$saveBtn = New-Object Windows.Forms.Button
$saveBtn.Text = 'Save card to image...'; $saveBtn.Location = New-Object Drawing.Point(16, 124); $saveBtn.Size = New-Object Drawing.Size(172, 40)
$form.Controls.Add($saveBtn)

$writeBtn = New-Object Windows.Forms.Button
$writeBtn.Text = 'Write image to card...'; $writeBtn.Location = New-Object Drawing.Point(194, 124); $writeBtn.Size = New-Object Drawing.Size(172, 40)
$form.Controls.Add($writeBtn)

$growBtn = New-Object Windows.Forms.Button
$growBtn.Text = 'Grow to fill card...'; $growBtn.Location = New-Object Drawing.Point(372, 124); $growBtn.Size = New-Object Drawing.Size(172, 40)
$form.Controls.Add($growBtn)

$bar = New-Object Windows.Forms.ProgressBar
$bar.Location = New-Object Drawing.Point(16, 180); $bar.Size = New-Object Drawing.Size(528, 22); $bar.Maximum = 1000
$form.Controls.Add($bar)

$status = New-Object Windows.Forms.Label
$status.Location = New-Object Drawing.Point(16, 208); $status.Size = New-Object Drawing.Size(528, 56)
$form.Controls.Add($status)

$cancelBtn = New-Object Windows.Forms.Button
$cancelBtn.Text = 'Stop'; $cancelBtn.Location = New-Object Drawing.Point(432, 290); $cancelBtn.Size = New-Object Drawing.Size(112, 40); $cancelBtn.Enabled = $false
$form.Controls.Add($cancelBtn)

$script:cards = @()
$script:stop = $false
$script:busy = $false

function Load-Cards([bool]$keepStatus = $false) {
    $combo.Items.Clear()
    $script:cards = @(Get-Cards)
    foreach ($c in $script:cards) { [void]$combo.Items.Add($c.Label) }
    if ($script:cards.Count) { $combo.SelectedIndex = 0; if (-not $keepStatus) { $status.Text = 'Pick the card, then save it or write an image to it.' } }
    else { $status.Text = "No card found. Windows sees: $(Disk-Summary). If the card isn't there, try another reader or slot; if it is, tick 'Show all drives'." }
}

function Set-Busy([bool]$on) {
    $script:busy = $on
    $saveBtn.Enabled = -not $on; $writeBtn.Enabled = -not $on; $growBtn.Enabled = -not $on; $refresh.Enabled = -not $on; $combo.Enabled = -not $on; $compress.Enabled = -not $on; $showAll.Enabled = -not $on; $quick.Enabled = -not $on
    $cancelBtn.Enabled = $on
}

function Show-Progress([int64]$done, [int64]$total, $sw, [string]$verb) {
    $bar.Value = [Math]::Min(1000, [int](1000.0 * $done / [Math]::Max([double]1, [double]$total)))
    $secs = [Math]::Max(0.1, $sw.Elapsed.TotalSeconds)
    $speed = $done / $secs
    $left = if ($speed -gt 0) { ($total - $done) / $speed } else { 0 }
    $status.Text = "$verb $(Format-Size $done) of $(Format-Size $total)  -  $(Format-Size $speed)/s  -  about $(Format-Time $left) left"
    [Windows.Forms.Application]::DoEvents()
}

function Selected-Card {
    if ($combo.SelectedIndex -lt 0) { return $null }
    $script:cards[$combo.SelectedIndex]
}

# Reads as much as asked for (streams can return less than asked)
function Read-Full($stream, [byte[]]$buf, [int]$want) {
    $got = 0
    while ($got -lt $want) {
        $n = $stream.Read($buf, $got, $want - $got)
        if ($n -le 0) { break }
        $got += $n
    }
    $got
}

$refresh.Add_Click({ Load-Cards })
$cancelBtn.Add_Click({ $script:stop = $true; $status.Text = 'Stopping...' })
$form.Add_FormClosing({ param($s, $e) if ($script:busy) { $script:stop = $true; $e.Cancel = $true } })

# ---- card -> image
$saveBtn.Add_Click({
    $card = Selected-Card
    if (-not $card) { return }
    $dlg = New-Object Windows.Forms.SaveFileDialog
    $ext = if ($compress.Checked) { 'img.gz' } else { 'img' }
    $dlg.FileName = "sd-card-$(Get-Date -Format yyyy-MM-dd).$ext"
    $dlg.Filter = if ($compress.Checked) { 'Compressed card image (*.img.gz)|*.img.gz' } else { 'Card image (*.img)|*.img' }
    $dlg.InitialDirectory = [Environment]::GetFolderPath('MyDocuments')
    if ($dlg.ShowDialog() -ne 'OK') { return }
    $outPath = $dlg.FileName
    $drive = [IO.Path]::GetPathRoot($outPath)
    try { $free = (New-Object IO.DriveInfo($drive)).AvailableFreeSpace } catch { $free = -1 }
    if (-not $compress.Checked -and $free -ge 0 -and $free -lt $card.Size) {
        [void][Windows.Forms.MessageBox]::Show("There isn't room on $drive for a $(Format-Size $card.Size) image. Tick 'Compress', or pick another drive.", 'SD Card Imager'); return
    }
    Set-Busy $true; $script:stop = $false
    $disk = $null; $out = $null; $gz = $null; $ok = $false
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
        $disk = [RawDisk]::OpenDisk($card.Number, $false)
        $map = $null
        if ($quick.Checked) {
            $status.Text = 'Reading the card''s map of used space...'; [Windows.Forms.Application]::DoEvents()
            $map = [FatMap]::Probe($disk, [int64]$card.Size)
            [void]$disk.Seek(0, 'Begin')
        }
        $out = [IO.File]::Create($outPath)
        $dest = $out
        if ($compress.Checked) { $gz = New-Object IO.Compression.GZipStream($out, [IO.Compression.CompressionLevel]::Fastest); $dest = $gz }
        else { [RawDisk]::SetSparse($out) }
        $buf = New-Object byte[] $CHUNK
        $zero = New-Object byte[] $CHUNK
        [int64]$done = 0
        $verb = if ($map) { "Saving (only the $(Format-Size $map.UsedBytes()) in use)" } else { 'Saving' }
        while ($done -lt [int64]$card.Size -and -not $script:stop) {
            $want = [int][Math]::Min([int64]$CHUNK, [int64]$card.Size - $done)   # (both as 64-bit: card sizes don't fit 32)
            if ($map -and -not $map.Needed($done, $want)) {
                # empty space: nothing to read; zeros in the image (a hole in an uncompressed one)
                [void]$disk.Seek($want, 'Current')
                if ($gz) { $gz.Write($zero, 0, $want) } else { [void]$out.Seek($want, 'Current') }
                $n = $want
            } else {
                $n = Read-Full $disk $buf $want
                if ($n -le 0) { throw "The card stopped answering after $(Format-Size $done)." }
                $dest.Write($buf, 0, $n)
            }
            $done += $n
            Show-Progress $done ([int64]$card.Size) $sw $verb
        }
        if (-not $gz) { $out.SetLength([int64]$card.Size) }
        $ok = -not $script:stop
    } catch {
        [void][Windows.Forms.MessageBox]::Show("Saving stopped: $($_.Exception.Message)", 'SD Card Imager')
    } finally {
        if ($gz) { $gz.Dispose() }
        if ($out) { $out.Dispose() }
        if ($disk) { $disk.Dispose() }
    }
    if ($ok) {
        $size = (Get-Item $outPath).Length
        $status.Text = "Complete. Saved $outPath  ($(Format-Size $size), in $(Format-Time $sw.Elapsed.TotalSeconds))"
        $bar.Value = 0
    } else {
        Remove-Item $outPath -ErrorAction SilentlyContinue
        $status.Text = 'Stopped. The unfinished image was deleted.'; $bar.Value = 0
    }
    Set-Busy $false
})

# Takes every volume on the card away from Windows while its raw sectors change
function Lock-CardVolumes($card) {
    $locks = @()
    foreach ($p in @(Get-Partition -DiskNumber $card.Number -ErrorAction SilentlyContinue)) {
        $v = $p | Get-Volume -ErrorAction SilentlyContinue
        if ($v -and $v.Path) { $locks += [RawDisk]::LockVolume(($v.Path.TrimEnd('\') -replace '^\\\\\?\\', '\\.\')) }
    }
    $locks
}

# ---- grow the last partition to fill the card
# $afterWrite: called straight after writing an image; then it only speaks up if there's space to gain.
function Grow-Card($card, [bool]$afterWrite) {
    $disk = $null
    try { $disk = [RawDisk]::OpenDisk($card.Number, $false); $plan = [CardGrower]::Check($disk, [int64]$card.Size) }
    catch { if (-not $afterWrite) { [void][Windows.Forms.MessageBox]::Show("Couldn't read the card: $($_.Exception.Message)", 'SD Card Imager') }; return }
    finally { if ($disk) { $disk.Dispose() } }

    if (-not $plan.CanGrow) {
        if (-not $afterWrite) { [void][Windows.Forms.MessageBox]::Show($plan.Message, 'SD Card Imager') }
        return
    }
    $old = Format-Size ($plan.OldSectors * 512); $new = Format-Size ($plan.NewSectors * 512)
    $intro = if ($afterWrite) { "The image only uses part of this card. " } else { '' }
    switch ($plan.Kind) {
        'fat32' { $what = "${intro}Grow the FAT32 partition from $old to $new, so the whole card can be used?`n`nYour files stay where they are. It only takes a minute. Keep the image you wrote until you've checked the card, just in case." }
        'linux' { $what = "${intro}Grow the card's Linux (Raspberry Pi) partition from $old to $new?`n`nWindows can make the partition bigger but can't touch the files inside it, so after this, boot the Pi from the card once and run this in a Terminal:`n`n    sudo resize2fs /dev/mmcblk0p2`n`n(It takes a few seconds; no reboot needed.)" }
        'ntfs'  { $what = "${intro}Grow the NTFS partition from $old to $new?" }
    }
    if ([Windows.Forms.MessageBox]::Show($what, 'SD Card Imager', 'YesNo', 'Question') -ne 'Yes') { return }

    Set-Busy $true; $cancelBtn.Enabled = $false
    $locks = @(); $disk = $null; $ok = $false
    try {
        if ($plan.Kind -eq 'ntfs') {
            $status.Text = 'Growing the partition...'; [Windows.Forms.Application]::DoEvents()
            $max = (Get-PartitionSupportedSize -DiskNumber $card.Number -PartitionNumber $plan.PartNumber).SizeMax
            Resize-Partition -DiskNumber $card.Number -PartitionNumber $plan.PartNumber -Size $max -ErrorAction Stop
        } else {
            $status.Text = 'Getting the card ready...'; [Windows.Forms.Application]::DoEvents()
            $locks = @(Lock-CardVolumes $card)
            $disk = [RawDisk]::OpenDisk($card.Number, $true)
            $say = [Action[string]]{ param($m) $status.Text = $m; [Windows.Forms.Application]::DoEvents() }
            [CardGrower]::Grow($disk, $plan, $say)
            $disk.Flush()
        }
        $ok = $true
    } catch {
        [void][Windows.Forms.MessageBox]::Show("Growing stopped: $($_.Exception.Message)", 'SD Card Imager')
    } finally {
        if ($disk) { $disk.Dispose() }
        foreach ($l in $locks) { $l.Dispose() }
    }
    try { Update-Disk -Number $card.Number -ErrorAction SilentlyContinue } catch {}
    Set-Busy $false
    if ($ok) {
        $status.Text = switch ($plan.Kind) {
            'linux' { "Done: the Linux partition is now $new. On the Pi, run:  sudo resize2fs /dev/mmcblk0p2" }
            default { "Done: the card now uses all $new. Take it out and put it back in before using it." }
        }
    } else { $status.Text = 'Growing stopped. If the card looks wrong, write the image to it again.' }
    Load-Cards $true
}

$growBtn.Add_Click({ $card = Selected-Card; if ($card) { Grow-Card $card $false } })

# ---- image -> card
$writeBtn.Add_Click({
    $card = Selected-Card
    if (-not $card) { return }
    $dlg = New-Object Windows.Forms.OpenFileDialog
    $dlg.Filter = 'Card images (*.img;*.img.gz)|*.img;*.img.gz|All files (*.*)|*.*'
    $dlg.InitialDirectory = [Environment]::GetFolderPath('MyDocuments')
    if ($dlg.ShowDialog() -ne 'OK') { return }
    $inPath = $dlg.FileName
    $zipped = $inPath.ToLower().EndsWith('.gz')
    $fileSize = (Get-Item $inPath).Length
    if (-not $zipped -and $fileSize -gt $card.Size) {
        [void][Windows.Forms.MessageBox]::Show("That image ($(Format-Size $fileSize)) is bigger than this card ($(Format-Size $card.Size)). Use a card at least as big as the one it came from.", 'SD Card Imager'); return
    }
    $msg = "Everything on this card will be replaced by the image:`n`n    $($card.Label)`n`nMake sure it's the right card. Continue?"
    if ($card.Size -gt 300GB) { $msg = "This drive is big for an SD card:`n`n    $($card.Label)`n`nEverything on it will be replaced. Are you SURE it's the card?" }
    if ([Windows.Forms.MessageBox]::Show($msg, 'SD Card Imager', 'YesNo', 'Warning') -ne 'Yes') { return }

    Set-Busy $true; $script:stop = $false
    $locks = @(); $disk = $null; $in = $null; $gz = $null; $ok = $false; $script:grewAfter = $null
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
        $status.Text = 'Getting the card ready...'; [Windows.Forms.Application]::DoEvents()
        foreach ($p in @(Get-Partition -DiskNumber $card.Number -ErrorAction SilentlyContinue)) {
            $v = $p | Get-Volume -ErrorAction SilentlyContinue
            if ($v -and $v.Path) { $locks += [RawDisk]::LockVolume(($v.Path.TrimEnd('\') -replace '^\\\\\?\\', '\\.\')) }
        }
        $disk = [RawDisk]::OpenDisk($card.Number, $true)
        $in = [IO.File]::OpenRead($inPath)
        $src = $in
        if ($zipped) { $gz = New-Object IO.Compression.GZipStream($in, [IO.Compression.CompressionMode]::Decompress); $src = $gz }
        $buf = New-Object byte[] $CHUNK
        [int64]$done = 0
        $total = if ($zipped) { $card.Size } else { $fileSize }
        $head = New-Object HeadCollector
        if (-not $quick.Checked) { $head.Done = $true }
        while (-not $script:stop) {
            $n = Read-Full $src $buf $CHUNK
            if ($n -le 0) { break }
            $pad = ($n % 512)
            if ($pad) { [Array]::Clear($buf, $n, 512 - $pad); $n += 512 - $pad }   # cards take whole sectors
            $head.Add($buf, $n)
            if ($head.Map -and -not $head.Map.Needed($done, $n)) {
                # empty space in the image: skip it on the card too
                if ($done + $n -le [int64]$card.Size) { [void]$disk.Seek($n, 'Current') }
            } else {
                if ($done + $n -gt [int64]$card.Size) { throw "The image is bigger than this card; it doesn't fit." }
                $disk.Write($buf, 0, $n)
            }
            $done += $n
            Show-Progress ($(if ($zipped) { $in.Position } else { $done })) ($(if ($zipped) { $fileSize } else { $total })) $sw 'Writing'
        }
        $disk.Flush()
        $ok = -not $script:stop
    } catch {
        [void][Windows.Forms.MessageBox]::Show("Writing stopped: $($_.Exception.Message)", 'SD Card Imager')
    } finally {
        if ($gz) { $gz.Dispose() }
        if ($in) { $in.Dispose() }
        if ($disk) { $disk.Dispose() }
        foreach ($l in $locks) { $l.Dispose() }
    }
    try { Update-Disk -Number $card.Number -ErrorAction SilentlyContinue } catch {}
    if ($ok) {
        $status.Text = "Complete. Written in $(Format-Time $sw.Elapsed.TotalSeconds). Take the card out and put it back in before using it."
        $bar.Value = 0
        $script:grewAfter = $card
    } else {
        $status.Text = 'Stopped part way: the card is incomplete. Write the image again before using it.'; $bar.Value = 0
    }
    Set-Busy $false
    Load-Cards $true
    if ($script:grewAfter) { $c = $script:grewAfter; $script:grewAfter = $null; Grow-Card $c $true }
})

Load-Cards
[void]$form.ShowDialog()
