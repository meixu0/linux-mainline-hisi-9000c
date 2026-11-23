/*
 * charlotte-diag: minimal bring-up helper for the Kirin 9000C (6.18 port).
 *
 * Purpose: inspect DPU/display MMIO and the EFI framebuffer from an
 * initramfs, reporting over the serial console via /dev/kmsg (the boot
 * console is kept alive with keep_bootcon).
 *
 * /dev/mem read()/write() is rejected for MMIO on arm64 (valid_phys_addr_range
 * requires memblock memory), but mmap() is allowed, so everything here mmaps.
 *
 * Build (native, static):
 *   gcc -static -O2 -o charlotte-diag charlotte-diag.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#define PAGE 4096UL
#define TAG  "<6>"      /* KERN_INFO */

static int kfd = -1;
static int emitted;
static int emit_max = 500;

static int usage(const char *what);

static void out(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(buf) - 1)
		n = sizeof(buf) - 1;

	if (emitted > emit_max)
		return;
	if (emitted == emit_max)
		n = snprintf(buf, sizeof(buf),
			     "... output limit (%d lines) reached", emit_max);
	emitted++;

	fwrite(buf, 1, n, stdout);
	fflush(stdout);

	if (kfd >= 0) {
		char k[1100];
		int m = snprintf(k, sizeof(k), TAG "%s\n", buf);
		if (write(kfd, k, m) < 0)
			;
	}
}

static void *map_phys(uint64_t phys, size_t len, int write)
{
	uint64_t base = phys & ~(PAGE - 1);
	size_t off = phys - base;
	size_t maplen = (off + len + PAGE - 1) & ~(PAGE - 1);
	int prot = PROT_READ | (write ? PROT_WRITE : 0);
	void *p;
	int fd;

	fd = open("/dev/mem", (write ? O_RDWR : O_RDONLY) | O_SYNC);
	if (fd < 0) {
		out("open /dev/mem: %s", strerror(errno));
		return NULL;
	}
	p = mmap(NULL, maplen, prot, MAP_SHARED, fd, base);
	close(fd);
	if (p == MAP_FAILED) {
		out("mmap %#llx+%#zx: %s", (unsigned long long)phys, len,
		    strerror(errno));
		return NULL;
	}
	return (char *)p + off;
}

static uint64_t parse(const char *s)
{
	return strtoull(s, NULL, 0);
}

/* per-4K block: nonzero count + FNV-1a hash, so dumps can be diffed */
static int cmd_blocks(int argc, char **argv)
{
	uint64_t base, len;
	volatile uint32_t *p;
	uint64_t off, blk;

	if (argc < 4)
		return usage("blocks <phys> <len>");
	base = parse(argv[2]);
	len = parse(argv[3]);
	p = map_phys(base, len, 0);
	if (!p)
		return 1;

	out("blocks %#llx..%#llx (4K blocks: off nz hash)",
	    (unsigned long long)base, (unsigned long long)(base + len));
	for (blk = 0; blk < len; blk += PAGE) {
		uint32_t nz = 0;
		uint32_t h = 2166136261u;
		uint64_t n = (len - blk < PAGE) ? (len - blk) : PAGE;

		for (off = 0; off < n; off += 4) {
			uint32_t v = p[(blk + off) / 4];

			if (v)
				nz++;
			h ^= v;
			h *= 16777619u;
		}
		if (nz)
			out("  %#06llx nz=%u h=%08x", (unsigned long long)blk, nz, h);
	}
	return 0;
}

/* list words whose value is inside [lo,hi] or exactly one of the extras */
static int cmd_ranges(int argc, char **argv)
{
	uint64_t base, len, lo, hi, off;
	volatile uint32_t *p;

	if (argc < 6)
		return usage("ranges <phys> <len> <lo> <hi>");
	base = parse(argv[2]);
	len = parse(argv[3]);
	lo = parse(argv[4]);
	hi = parse(argv[5]);
	p = map_phys(base, len, 0);
	if (!p)
		return 1;

	out("ranges %#llx..%#llx in [%#llx,%#llx]",
	    (unsigned long long)base, (unsigned long long)(base + len),
	    (unsigned long long)lo, (unsigned long long)hi);
	for (off = 0; off < len; off += 4) {
		uint32_t v = p[off / 4];

		if (v >= lo && v <= hi)
			out("  %#06llx = %#010x", (unsigned long long)off, v);
	}
	return 0;
}

static int cmd_find(int argc, char **argv)
{
	uint64_t base, len, want, off;
	volatile uint32_t *p;

	if (argc < 5)
		return usage("find <phys> <len> <value>");
	base = parse(argv[2]);
	len = parse(argv[3]);
	want = parse(argv[4]);
	p = map_phys(base, len, 0);
	if (!p)
		return 1;

	out("find %#llx in %#llx..%#llx", (unsigned long long)want,
	    (unsigned long long)base, (unsigned long long)(base + len));
	for (off = 0; off < len; off += 4) {
		uint32_t v = p[off / 4];

		if (v == want)
			out("  %#06llx = %#010x", (unsigned long long)off, v);
	}
	return 0;
}

static int cmd_dump(int argc, char **argv)
{
	uint64_t base, len, off;
	volatile uint32_t *p;

	if (argc < 4)
		return usage("dump <phys> <len>");
	base = parse(argv[2]);
	len = parse(argv[3]);
	p = map_phys(base, len, 0);
	if (!p)
		return 1;

	for (off = 0; off < len; off += 16) {
		out("  %#08llx: %08x %08x %08x %08x",
		    (unsigned long long)(base + off),
		    p[off / 4], p[off / 4 + 1], p[off / 4 + 2], p[off / 4 + 3]);
	}
	return 0;
}

static int cmd_fill(int argc, char **argv)
{
	uint64_t base, len, val, off;
	volatile uint32_t *p;

	if (argc < 5)
		return usage("fill <phys> <len> <value32>");
	base = parse(argv[2]);
	len = parse(argv[3]);
	val = parse(argv[4]);
	p = map_phys(base, len, 1);
	if (!p)
		return 1;

	for (off = 0; off < len; off += 4)
		p[off / 4] = (uint32_t)val;
	out("filled %#llx..%#llx with %#010x",
	    (unsigned long long)base, (unsigned long long)(base + len),
	    (unsigned)val);
	return 0;
}

/* color bars: 8 vertical bars, so orientation/scaling is visible too */
static int cmd_fbbars(int argc, char **argv)
{
	static const uint32_t colors[8] = {
		0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff,
		0xffffff00, 0xff00ffff, 0xffff00ff, 0xff000000,
	};
	uint32_t w = 1920, h = 1080, stride;
	int fd, i;
	void *fb;
	size_t len;
	uint32_t *row;

	(void)argc; (void)argv;
	if (argc >= 4) {
		w = parse(argv[2]);
		h = parse(argv[3]);
	}
	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		out("open /dev/fb0: %s", strerror(errno));
		return 1;
	}
	/* read stride/bpp from sysfs when possible */
	stride = w * 4;
	{
		FILE *f = fopen("/sys/class/graphics/fb0/stride", "r");
		if (f) {
			unsigned long v;
			if (fscanf(f, "%lu", &v) == 1 && v)
				stride = v;
			fclose(f);
		}
		f = fopen("/sys/class/graphics/fb0/virtual_size", "r");
		if (f) {
			unsigned long vw, vh;
			if (fscanf(f, "%lu,%lu", &vw, &vh) == 2)
				out("fb0 virtual_size=%lux%lu stride=%u", vw, vh, stride);
			fclose(f);
		}
	}
	len = (size_t)stride * h;
	fb = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		out("mmap fb0: %s", strerror(errno));
		close(fd);
		return 1;
	}
	row = fb;
	for (i = 0; i < (int)h; i++) {
		uint32_t *px = row + (size_t)i * (stride / 4);
		uint32_t x;
		for (x = 0; x < w; x++)
			px[x] = colors[(x * 8) / w];
	}
	msync(fb, len, MS_SYNC);
	out("wrote 8 color bars (%ux%u, stride %u) to /dev/fb0", w, h, stride);
	munmap(fb, len);
	close(fd);
	return 0;
}

static int cmd_fbinfo(int argc, char **argv)
{
	int fd;
	void *fb;
	size_t len = 1920 * 1080 * 4;
	uint32_t *p;
	size_t i, nz = 0, samples = 0;
	uint32_t colors[8] = {0};
	char modes[256] = "";

	(void)argc; (void)argv;
	fd = open("/dev/fb0", O_RDONLY);
	if (fd < 0) {
		out("open /dev/fb0: %s", strerror(errno));
		return 1;
	}
	{
		FILE *f = fopen("/sys/class/graphics/fb0/modes", "r");
		if (f) {
			if (fgets(modes, sizeof(modes), f))
				modes[strcspn(modes, "\n")] = 0;
			fclose(f);
		}
	}
	fb = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		out("mmap fb0: %s", strerror(errno));
		close(fd);
		return 1;
	}
	p = fb;
	for (i = 0; i < len / 4; i++) {
		if (p[i])
			nz++;
	}
	/* sample distinct colors */
	for (i = 0; i < len / 4; i += 37) {
		uint32_t v = p[i];
		int j, best = -1;

		if (!v)
			continue;
		for (j = 0; j < 8; j++) {
			if (colors[j] == v)
				break;
			if (colors[j] == 0)
				best = j;
		}
		if (j < 8)
			continue;
		if (best >= 0)
			colors[best] = v;
		samples++;
	}
	out("fb0 mode=%s nonzero_pixels=%zu/%zu sampled_nonzero=%zu",
	    modes, nz, len / 4, samples);
	out("fb0 colors seen: %08x %08x %08x %08x %08x %08x %08x %08x",
	    colors[0], colors[1], colors[2], colors[3],
	    colors[4], colors[5], colors[6], colors[7]);
	munmap(fb, len);
	close(fd);
	return 0;
}

static int usage(const char *what)
{
	out("usage: %s", what);
	return 2;
}

int main(int argc, char **argv)
{
	kfd = open("/dev/kmsg", O_WRONLY);
	if (kfd < 0)
		kfd = -1;

	if (argc < 2)
		return usage("dump|blocks|ranges|find|fill|fbbars|fbinfo ...");

	if (!strcmp(argv[1], "dump"))
		return cmd_dump(argc, argv);
	if (!strcmp(argv[1], "blocks"))
		return cmd_blocks(argc, argv);
	if (!strcmp(argv[1], "ranges"))
		return cmd_ranges(argc, argv);
	if (!strcmp(argv[1], "find"))
		return cmd_find(argc, argv);
	if (!strcmp(argv[1], "fill"))
		return cmd_fill(argc, argv);
	if (!strcmp(argv[1], "fbbars"))
		return cmd_fbbars(argc, argv);
	if (!strcmp(argv[1], "fbinfo"))
		return cmd_fbinfo(argc, argv);
	return usage("unknown command");
}
