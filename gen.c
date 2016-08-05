/* neatcc code generation */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "ncc.h"

static struct mem ds;		/* data segment */
static struct mem cs;		/* code segment */
static long bsslen;		/* bss segment size */

static long *loc_off;		/* offset of locals on the stack */
static long loc_n, loc_sz;	/* number of locals */
static long loc_pos;		/* current stack position */
static int *loc_mem;		/* local memory was accessed */
static int *loc_ptr;		/* the address of this local is fetched */
static int *loc_dat;		/* the number of data accesses of this local */

static char (*ds_name)[NAMELEN];/* data section symbols */
static long *ds_off;		/* data section offsets */
static long ds_n, ds_sz;	/* number of data section symbols */

static int func_argc;		/* number of arguments */
static int func_varg;		/* varargs */
static int func_regs;		/* used registers */
static int func_maxargs;	/* the maximum number of arguments on the stack */
static int func_leaf;		/* a leaf function */
static long *ic_bbeg;		/* whether each instruction begins a basic block */

static long ra_vmap[N_REGS];	/* register to intermediate value assignments */
static long ra_lmap[N_REGS];	/* register to local assignments */
static long ra_lmapglob[N_REGS];	/* global register to local assignments */
static long *ra_gmask;		/* the mask of good registers for each value */
static long ra_live[NTMPS];	/* live values */
static int ra_vmax;		/* the number of values stored on the stack */

static long loc_add(long pos)
{
	if (loc_n >= loc_sz) {
		loc_sz = MAX(128, loc_sz * 2);
		loc_off = mextend(loc_off, loc_n, loc_sz, sizeof(loc_off[0]));
	}
	loc_off[loc_n] = pos;
	return loc_n++;
}

long o_mklocal(long sz)
{
	loc_pos += ALIGN(sz, ULNG);
	return loc_add(loc_pos);
}

void o_rmlocal(long addr, long sz)
{
}

long o_arg2loc(int i)
{
	return i;
}

void o_bsnew(char *name, long size, int global)
{
	out_def(name, OUT_BSS | (global ? OUT_GLOB : 0), bsslen, size);
	bsslen += ALIGN(size, OUT_ALIGNMENT);
}

long o_dsnew(char *name, long size, int global)
{
	int idx;
	if (ds_n >= ds_sz) {
		ds_sz = MAX(128, ds_sz * 2);
		ds_name = mextend(ds_name, ds_n, ds_sz, sizeof(ds_name[0]));
		ds_off = mextend(ds_off, ds_n, ds_sz, sizeof(ds_off[0]));
	}
	idx = ds_n++;
	strcpy(ds_name[idx], name);
	ds_off[idx] = mem_len(&ds);
	out_def(name, OUT_DS | (global ? OUT_GLOB : 0), mem_len(&ds), size);
	mem_putz(&ds, ALIGN(size, OUT_ALIGNMENT));
	return ds_off[idx];
}

void o_dscpy(long addr, void *buf, long len)
{
	mem_cpy(&ds, addr, buf, len);
}

static int dat_off(char *name)
{
	int i;
	for (i = 0; i < ds_n; i++)
		if (!strcmp(name, ds_name[i]))
			return ds_off[i];
	return 0;
}

void o_dsset(char *name, long off, long bt)
{
	long sym_off = dat_off(name) + off;
	long num, roff, rsym;
	if (!o_popnum(&num)) {
		mem_cpy(&ds, sym_off, &num, T_SZ(bt));
		return;
	}
	if (!o_popsym(&rsym, &roff)) {
		out_rel(rsym, OUT_DS, sym_off);
		mem_cpy(&ds, sym_off, &roff, T_SZ(bt));
	}
}

static int ra_vreg(int val)
{
	int i;
	for (i = 0; i < LEN(ra_vmap); i++)
		if (ra_vmap[i] == val)
			return i;
	return -1;
}

static int ra_lreg(int loc)
{
	int i;
	for (i = 0; i < LEN(ra_lmap); i++)
		if (ra_lmap[i] == loc)
			return i;
	return -1;
}

/* mask of registers assigned to locals */
static long ra_lmask(void)
{
	long m = 0;
	int i;
	for (i = 0; i < LEN(ra_lmap); i++)
		if (ra_lmap[i] >= 0)
			m |= (1 << i);
	return m;
}

/* mask of registers assigned to values */
static long ra_vmask(void)
{
	long m = 0;
	int i;
	for (i = 0; i < LEN(ra_vmap); i++)
		if (ra_vmap[i] >= 0)
			m |= (1 << i);
	return m;
}

/* find a temporary register specified in the given mask */
static long ra_regscn(long mask)
{
	int i;
	for (i = 0; i < N_TMPS; i++)
		if ((1 << tmpregs[i]) & mask)
			return tmpregs[i];
	return -1;
}

/* find a register, with the given good, acceptable, and bad register masks */
static long ra_regget(long iv, long gmask, long amask, long bmask)
{
	long lmask, vmask;
	gmask &= ~bmask & amask;
	amask &= ~bmask;
	if (ra_vreg(iv) >= 0 && (1 << ra_vreg(iv)) & (gmask | amask))
		return ra_vreg(iv);
	vmask = ra_vmask();
	lmask = ra_lmask();
	if (ra_regscn(gmask & ~vmask & ~lmask) >= 0)
		return ra_regscn(gmask & ~vmask & ~lmask);
	if (ra_regscn(amask & ~vmask & ~lmask) >= 0)
		return ra_regscn(amask & ~vmask & ~lmask);
	if (ra_regscn(gmask) >= 0)
		return ra_regscn(gmask);
	if (ra_regscn(amask) >= 0)
		return ra_regscn(amask);
	die("neatcc: cannot allocate an acceptable register\n");
	return 0;
}

/* find a free and cheap register */
static long ra_regcheap(long mask)
{
	return ra_regscn(mask & (func_regs | (R_TMPS & ~R_PERM)) &
			~ra_lmask() & ~ra_vmask());
}

/* allocate registers for a 3-operand instruction */
static void ra_map(struct ic *ic, int *r0, int *r1, int *r2, long *mt)
{
	long m0, m1, m2;
	long all = 0;
	int n = ic_regcnt(ic);
	int oc = O_C(ic->op);
	int i;
	*r0 = -1;
	*r1 = -1;
	*r2 = -1;
	*mt = 0;
	/* optimizing loading locals: point to local's register */
	if (oc == (O_LD | O_LOC) && ra_lreg(ic->arg1) >= 0 &&
			ra_vmap[ra_lreg(ic->arg1)] < 0) {
		*r0 = ra_lreg(ic->arg1);
		func_regs |= 1 << *r0;
		return;
	}
	/* do not use argument registers to hold call destination */
	if (oc & O_CALL)
		for (i = 0; i < MIN(ic->arg2, N_ARGS); i++)
			all |= (1 << argregs[i]);
	/* instructions on locals can be simplified */
	if (oc & O_LOC) {
		if (oc & O_MOV)
			oc = O_ADD | O_NUM;
		if (oc & (O_ST | O_LD))
			oc = (oc & ~O_LOC) & O_NUM;
	}
	if (i_reg(ic->op, &m0, &m1, &m2, mt))
		die("neatcc: instruction %08lx not supported\n", ic->op);
	/*
	 * the registers used in global register allocation should not
	 * be used in the last instruction of a basic block.
	 */
	if (ic->op & (O_JZ | O_JCC))
		for (i = 0; i < LEN(ra_lmap); i++)
			if (ra_lmapglob[i] >= 0 && ra_lmap[i] != ra_lmapglob[i])
				all |= (1 << i);
	/* allocating registers for the operands */
	if (n >= 3) {
		*r2 = ra_regget(ic->arg2, m2, m2, all);
		all |= (1 << *r2);
	}
	if (n >= 2) {
		*r1 = ra_regget(ic->arg1, m1, m1, all);
		all |= (1 << *r1);
	}
	if (n >= 1 && m0) {
		int wop = ic->op & O_OUT;
		if (wop && n >= 3 && m0 & (1 << *r2))
			*r0 = *r2;
		else if (wop && n >= 2 && m0 & (1 << *r1))
			*r0 = *r1;
		else
			*r0 = ra_regget(ic->arg0, ra_gmask[ic->arg0],
				m0, wop ? 0 : all);
	}
	if (n >= 1 && !m0)
		*r0 = *r1;
	/* if r0 is overwritten and it is a local; use another register */
	if (n >= 1 && oc & O_OUT && ra_lmap[*r0] >= 0) {
		long m3 = (m0 ? m0 : m1) & ~(all | (1 << *r0));
		long arg3 = m0 ? ic->arg0 : ic->arg1;
		if (m3 != 0) {
			int r3 = ra_regget(arg3, ra_gmask[ic->arg0], m3, 0);
			if (n >= 2 && *r0 == *r1)
				*r1 = r3;
			if (n >= 3 && *r0 == *r2)
				*r2 = r3;
			*r0 = r3;
		}
	}
	if (n)
		all |= (1 << *r0);
	func_regs |= all | *mt;
}

static long iv_rank(long iv)
{
	int i;
	for (i = 0; i < LEN(ra_live); i++)
		if (ra_live[i] == iv)
			return i;
	die("neatcc: the specified value is not live\n");
	return 0;
}

static long iv_addr(long rank)
{
	return loc_pos + rank * ULNG + ULNG;
}

static void loc_toreg(long loc, long off, int reg, int bt)
{
	loc_mem[loc]++;
	i_ins(O_MK(O_LD | O_NUM, bt), reg, REG_FP, -loc_off[loc] + off);
}

static void loc_tomem(long loc, long off, int reg, int bt)
{
	loc_mem[loc]++;
	i_ins(O_MK(O_ST | O_NUM, bt), reg, REG_FP, -loc_off[loc] + off);
}

static void loc_toadd(long loc, long off, int reg)
{
	loc_mem[loc]++;
	i_ins(O_ADD | O_NUM, reg, REG_FP, -loc_off[loc] + off);
}

static void val_toreg(long val, int reg)
{
	i_ins(O_MK(O_LD | O_NUM, ULNG), reg, REG_FP, -iv_addr(iv_rank(val)));
}

static void val_tomem(long val, int reg)
{
	long rank = iv_rank(val);
	ra_vmax = MAX(ra_vmax, rank + 1);
	i_ins(O_MK(O_ST | O_NUM, ULNG), reg, REG_FP, -iv_addr(rank));
}

/* move the value to the stack */
static void ra_spill(int reg)
{
	if (ra_vmap[reg] >= 0) {
		val_tomem(ra_vmap[reg], reg);
		ra_vmap[reg] = -1;
	}
	if (ra_lmap[reg] >= 0) {
		if (ra_lmap[reg] == ra_lmapglob[reg])
			loc_tomem(ra_lmap[reg], 0, reg, ULNG);
		ra_lmap[reg] = -1;
	}
}

/* set the value to the given register */
static void ra_vsave(long iv, int reg)
{
	int i;
	ra_vmap[reg] = iv;
	for (i = 0; i < LEN(ra_live); i++)
		if (ra_live[i] < 0)
			break;
	if (i == LEN(ra_live))
		die("neatcc: too many live values\n");
	ra_live[i] = iv;
}

/* load the value into a register */
static void ra_vload(long iv, int reg)
{
	if (ra_vmap[reg] == iv)
		return;
	if (ra_vmap[reg] >= 0 || ra_lmap[reg] >= 0)
		ra_spill(reg);
	if (ra_vreg(iv) >= 0) {
		i_ins(O_MK(O_MOV, ULNG), reg, ra_vreg(iv), 0);
		ra_vmap[ra_vreg(iv)] = -1;
	} else {
		val_toreg(iv, reg);
	}
	ra_vmap[reg] = iv;
}

/* the value is no longer needed */
static void ra_vdrop(long iv)
{
	int i;
	for (i = 0; i < LEN(ra_live); i++)
		if (ra_live[i] == iv)
			ra_live[i] = -1;
	if (ra_vreg(iv) >= 0)
		ra_vmap[ra_vreg(iv)] = -1;
}

/* move the given value to memory or a free register */
static void ra_vmove(long iv, long mask)
{
	int src = ra_vreg(iv);
	int dst = ra_regcheap(mask);
	if (dst >= 0 && ra_vmap[dst] < 0 && ra_lmap[dst] < 0) {
		i_ins(O_MK(O_MOV, ULNG), dst, src, 0);
		ra_vmap[dst] = iv;
	} else {
		val_tomem(iv, src);
	}
	ra_vmap[src] = -1;
}

/* load the value of local loc into register reg */
static void ra_lload(long loc, long off, int reg, int bt)
{
	if (ra_lreg(loc) < 0 && !loc_ptr[loc] && loc_dat[loc] > 2) {
		ra_lmap[reg] = loc;
		loc_toreg(loc, off, reg, bt);
	}
	if (ra_lreg(loc) >= 0) {
		if (ra_lreg(loc) != reg)
			i_ins(O_MK(O_MOV, bt), reg, ra_lreg(loc), 0);
	} else {
		loc_toreg(loc, off, reg, bt);
	}
}

/* register reg contains the value of local loc */
static void ra_lsave(long loc, long off, int reg, int bt)
{
	int lreg = ra_lreg(loc);
	if (lreg >= 0 && ra_lmapglob[lreg] == ra_lmap[lreg]) {
		if (ra_vmap[lreg] >= 0)	/* values using the same register */
			ra_vmove(ra_vmap[lreg],
				ra_gmask[ra_vmap[lreg]] & ~(1 << reg));
		i_ins(O_MK(O_MOV, bt), lreg, reg, 0);
	} else {
		if (lreg >= 0)
			ra_lmap[lreg] = -1;
		loc_tomem(loc, off, reg, bt);
		if (!loc_ptr[loc] && loc_dat[loc] > 2 && ra_lmap[reg] < 0)
			ra_lmap[reg] = loc;
	}
}

/* end of a basic block */
static void ra_bbend(void)
{
	int i;
	/* save values to memory */
	for (i = 0; i < LEN(ra_vmap); i++)
		if (ra_vmap[i] >= 0)
			ra_spill(i);
	/* dropping local caches */
	for (i = 0; i < LEN(ra_lmap); i++)
		if (ra_lmap[i] != ra_lmapglob[i] && ra_lmap[i] >= 0)
			ra_spill(i);
	/* load global register allocations from memory */
	for (i = 0; i < LEN(ra_lmap); i++) {
		if (ra_lmap[i] != ra_lmapglob[i]) {
			ra_lmap[i] = ra_lmapglob[i];
			loc_toreg(ra_lmap[i], 0, i, ULNG);
		}
	}
}

/* perform global register allocation */
static void ra_glob(struct ic *ic, int ic_n)
{
	int *srt;
	long mask;
	int nregs;
	int i, j;
	mask = func_leaf ? R_TMPS : R_PERM;
	nregs = MIN(N_TMPS >> 1, 4);
	srt = malloc(loc_n * sizeof(srt[0]));
	/* sorting locals */
	for (i = 0; i < loc_n; i++) {
		for (j = i - 1; j >= 0 && loc_dat[i] > loc_dat[srt[j]]; j--)
			srt[j + 1] = srt[j];
		srt[j + 1] = i;
	}
	/* allocating registers */
	for (i = 0; i < loc_n && nregs > 0; i++) {
		int l = srt[i];
		if (loc_ptr[l])
			continue;
		if (func_leaf && l < N_ARGS && l < func_argc &&
				ra_lmapglob[argregs[l]] < 0) {
			ra_lmapglob[argregs[l]] = l;
			nregs--;
			continue;
		}
		if (loc_dat[l] < 2)
			continue;
		for (j = func_leaf ? 1 : 3; j < N_TMPS; j++) {
			int r = tmpregs[j];
			if (ra_lmapglob[r] < 0 && (1 << r) & mask) {
				ra_lmapglob[r] = l;
				nregs--;
				break;
			}
		}
	}
	free(srt);
}

static void ra_init(struct ic *ic, int ic_n)
{
	long m0, m1, m2, mt;
	int *loc_sz;
	int i, j;
	ic_bbeg = calloc(ic_n, sizeof(ic_bbeg[0]));
	ra_gmask = calloc(ic_n, sizeof(ra_gmask[0]));
	loc_mem = calloc(loc_n, sizeof(loc_mem[0]));
	/* ic_bbeg */
	for (i = 0; i < ic_n; i++) {
		if (i + 1 < ic_n && ic[i].op & (O_JXX | O_RET))
			ic_bbeg[i + 1] = 1;
		if (ic[i].op & O_JXX && ic[i].arg2 < ic_n)
			ic_bbeg[ic[i].arg2] = 1;
	}
	/* ra_gmask */
	for (i = 0; i < ic_n; i++) {
		int n = ic_regcnt(ic + i);
		int op = ic[i].op;
		i_reg(op, &m0, &m1, &m2, &mt);
		if (n >= 1 && !(op & O_OUT))
			ra_gmask[ic[i].arg0] = m0;
		if (n >= 2)
			ra_gmask[ic[i].arg1] = m1;
		if (n >= 3)
			ra_gmask[ic[i].arg2] = m2;
		if (op & O_CALL)
			for (j = 0; j < MIN(N_ARGS, ic[i].arg2); j++)
				ra_gmask[ic[i].args[j]] = 1 << argregs[j];
	}
	/* loc_ptr and loc_dat */
	loc_ptr = calloc(loc_n, sizeof(loc_ptr[0]));
	loc_dat = calloc(loc_n, sizeof(loc_dat[0]));
	loc_sz = calloc(loc_n, sizeof(loc_sz[0]));
	for (i = 0; i < ic_n; i++) {
		long oc = O_C(ic[i].op);
		if (oc == (O_LD | O_LOC) || oc == (O_ST | O_LOC)) {
			int loc = ic[i].arg1;
			int sz = T_SZ(O_T(ic[i].op));
			if (!loc_sz[loc])
				loc_sz[loc] = sz;
			if (ic[i].arg2 || sz < 2 || sz != loc_sz[loc])
				loc_ptr[loc]++;
			else
				loc_dat[loc]++;
		}
		if (oc == (O_MOV | O_LOC))
			loc_ptr[ic[i].arg1]++;
	}
	free(loc_sz);
	/* func_leaf */
	func_leaf = 1;
	for (i = 0; i < ic_n; i++)
		if (ic[i].op & O_CALL)
			func_leaf = 0;
	/* ra_vmap */
	for (i = 0; i < LEN(ra_vmap); i++)
		ra_vmap[i] = -1;
	/* ra_lmap */
	for (i = 0; i < N_REGS; i++)
		ra_lmapglob[i] = -1;
	ra_glob(ic, ic_n);
	memcpy(ra_lmap, ra_lmapglob, sizeof(ra_lmap));
	for (i = 0; i < LEN(ra_lmapglob); i++)
		if (ra_lmapglob[i] >= 0)
			func_regs |= (1 << i);
	/* ra_live */
	for (i = 0; i < LEN(ra_live); i++)
		ra_live[i] = -1;
	ra_vmax = 0;
	func_maxargs = 0;
}

static void ra_done(void)
{
	free(ic_bbeg);
	free(ra_gmask);
	free(loc_mem);
	free(loc_ptr);
	free(loc_dat);
}

static void ic_gencode(struct ic *ic, int ic_n)
{
	int r0, r1, r2;
	long mt;
	int i, j;
	long *ic_luse;
	ic_luse = ic_lastuse(ic, ic_n);
	/* loading arguments in their allocated registers */
	for (i = 0; i < LEN(ra_lmap); i++) {
		int loc = ra_lmap[i];
		if (loc >= 0 && loc < func_argc)
			if (loc >= N_ARGS || i != argregs[loc])
				loc_toreg(loc, 0, i, ULNG);
	}
	/* generating code */
	for (i = 0; i < ic_n; i++) {
		long op = ic[i].op;
		long oc = O_C(op);
		int n = ic_regcnt(ic + i);
		i_label(i);
		ra_map(ic + i, &r0, &r1, &r2, &mt);
		if (oc & O_CALL) {
			int argc = ic[i].arg2;
			int aregs = MIN(N_ARGS, argc);
			/* arguments passed via stack */
			for (j = argc - 1; j >= aregs; --j) {
				int v = ic[i].args[j];
				int rx = ra_vreg(v) >= 0 ? ra_vreg(v) : r0;
				ra_vload(v, rx);
				i_ins(O_MK(O_ST | O_NUM, ULNG), rx, REG_SP,
					(j - aregs) * ULNG);
				ra_vdrop(v);
			}
			func_maxargs = MAX(func_maxargs, argc - aregs);
			/* arguments passed via registers */
			for (j = aregs - 1; j >= 0; --j)
				ra_vload(ic[i].args[j], argregs[j]);
		}
		/* loading the operands */
		if (n >= 1 && !(oc & O_OUT))
			ra_vload(ic[i].arg0, r0);
		if (n >= 2 && !(oc & O_LOC))
			ra_vload(ic[i].arg1, r1);
		if (n >= 3)
			ra_vload(ic[i].arg2, r2);
		/* dropping values that are no longer used */
		for (j = 0; j < LEN(ra_live); j++)
			if (ra_live[j] >= 0 && ic_luse[ra_live[j]] <= i)
				ra_vdrop(ra_live[j]);
		/* saving values stored in registers that may change */
		for (j = 0; j < N_REGS; j++)
			if (mt & (1 << j))
				ra_spill(j);
		/* overwriting a value that is needed later (unless loading a local to its register) */
		if (n >= 1 && oc & O_OUT)
			if (oc != (O_LD | O_LOC) || ra_lmap[r0] != ic[i].arg1 ||
					ra_vmap[r0] >= 0)
				ra_spill(r0);
		/* before the last instruction of a basic block; for jumps */
		if (i + 1 < ic_n && ic_bbeg[i + 1] && oc & O_JXX)
			ra_bbend();
		/* performing the instruction */
		if (oc & O_BOP)
			i_ins(op, r0, r1, oc & O_NUM ? ic[i].arg2 : r2);
		if (oc & O_UOP)
			i_ins(op, r0, r1, r2);
		if (oc == (O_LD | O_NUM))
			i_ins(op, r0, r1, ic[i].arg2);
		if (oc == (O_LD | O_LOC))
			ra_lload(ic[i].arg1, ic[i].arg2, r0, O_T(op));
		if (oc == (O_ST | O_NUM))
			i_ins(op, r0, r1, ic[i].arg2);
		if (oc == (O_ST | O_LOC))
			ra_lsave(ic[i].arg1, ic[i].arg2, r0, O_T(op));
		if (oc == O_RET)
			i_ins(op, r0, 0, 0);
		if (oc == O_MOV)
			i_ins(op, r0, r0, 0);
		if (oc == (O_MOV | O_NUM))
			i_ins(op, r0, ic[i].arg1, 0);
		if (oc == (O_MOV | O_LOC))
			loc_toadd(ic[i].arg1, ic[i].arg2, r0);
		if (oc == (O_MOV | O_SYM))
			i_ins(op, r0, ic[i].arg1, ic[i].arg2);
		if (oc == O_CALL)
			i_ins(op, r0, r1, 0);
		if (oc == (O_CALL | O_SYM))
			i_ins(op, r0, ic[i].arg1, 0);
		if (oc == O_JMP)
			i_ins(op, 0, 0, ic[i].arg2);
		if (oc & O_JZ)
			i_ins(op, r0, 0, ic[i].arg2);
		if (oc & O_JCC)
			i_ins(op, r0, oc & O_NUM ? ic[i].arg1 : r1, ic[i].arg2);
		if (oc == O_MSET)
			i_ins(op, r0, r1, r2);
		if (oc == O_MCPY)
			i_ins(op, r0, r1, r2);
		/* saving back the output register */
		if (oc & O_OUT)
			ra_vsave(ic[i].arg0, r0);
		/* after the last instruction of a basic block */
		if (i + 1 < ic_n && ic_bbeg[i + 1] && !(oc & O_JXX))
			ra_bbend();
	}
	free(ic_luse);
	i_label(ic_n);
}

static void ic_reset(void)
{
	o_tmpdrop(-1);
	o_back(0);
	free(loc_off);
	loc_off = NULL;
	loc_n = 0;
	loc_sz = 0;
	loc_pos = I_LOC0;
}

void o_func_beg(char *name, int argc, int global, int varg)
{
	int i;
	func_argc = argc;
	func_varg = varg;
	func_regs = 0;
	ic_reset();
	for (i = 0; i < argc; i++)
		loc_add(I_ARG0 + -i * ULNG);
	out_def(name, (global ? OUT_GLOB : 0) | OUT_CS, mem_len(&cs), 0);
}

void o_code(char *name, char *c, long c_len)
{
	out_def(name, OUT_CS, mem_len(&cs), 0);
	mem_put(&cs, c, c_len);
}

void o_func_end(void)
{
	struct ic *ic;
	long ic_n, spsub;
	long sargs = 0;
	long sargs_last = -1;
	long sregs_pos;
	char *c;
	long c_len, *rsym, *rflg, *roff, rcnt;
	int locs = 0;			/* accessing locals on the stack */
	int i;
	ic_get(&ic, &ic_n);		/* the intermediate code */
	ra_init(ic, ic_n);		/* initialize register allocation */
	ic_gencode(ic, ic_n);		/* generating machine code */
	/* deciding which arguments to save */
	for (i = 0; i < func_argc; i++)
		if (loc_mem[i])
			sargs_last = i + 1;
	for (i = 0; i < N_ARGS && (func_varg || i < sargs_last); i++)
		sargs |= 1 << argregs[i];
	/* computing the amount of stack subtraction */
	for (i = 0; i < loc_n; i++)
		if (loc_mem[i])
			locs = 1;
	spsub = (locs || ra_vmax) ? loc_pos + ra_vmax * ULNG : 0;
	for (i = 0; i < N_TMPS; i++)
		if (((1 << tmpregs[i]) & func_regs & R_PERM) != 0)
			spsub += ULNG;
	sregs_pos = spsub;
	spsub += func_maxargs * ULNG;
	/* adding function prologue and epilogue */
	i_wrap(func_argc, sargs, spsub, spsub || locs || !func_leaf,
		func_regs & R_PERM, -sregs_pos);
	ra_done();
	i_code(&c, &c_len, &rsym, &rflg, &roff, &rcnt);
	for (i = 0; i < rcnt; i++)	/* adding the relocations */
		out_rel(rsym[i], rflg[i], roff[i] + mem_len(&cs));
	mem_put(&cs, c, c_len);		/* appending function code */
	free(c);
	free(rsym);
	free(rflg);
	free(roff);
	for (i = 0; i < ic_n; i++)
		ic_free(&ic[i]);
	free(ic);
	ic_reset();
}

void o_write(int fd)
{
	i_done();
	out_write(fd, mem_buf(&cs), mem_len(&cs), mem_buf(&ds), mem_len(&ds));
	free(loc_off);
	free(ds_name);
	free(ds_off);
	mem_done(&cs);
	mem_done(&ds);
}
