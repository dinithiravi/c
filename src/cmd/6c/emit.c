#include <u.h>
#include <ds/ds.h>
#include <cc/c.h>
#include <gc/gc.h>

static void expr(Node *);
static void stmt(Node *);
static void store(CTy *);
static void pushstruct(CTy *);

char    *intargregs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
int      stackoffset;
StkSlot *memretptr;
StkSlot *scratcharea;

static FILE *o;

void
emitinit(FILE *out)
{
	o = out;
}

static void
out(char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	if(vfprintf(o, fmt, va) < 0)
		errorf("Error printing\n");
	va_end(va);
}

static void
block(Node *n)
{
	Vec *v;
	int  i;

	v = n->Block.stmts;
	for(i = 0; i < v->len ; i++) {
		stmt(vecget(v, i));
	}
}


typedef enum {
	ARGINT1,
	ARGINT2,
	ARGMEM
} Argclass;

typedef struct {
	Argclass class;
	int offset;
	int r1;
	int r2;
} Argloc;

static Argclass
classify(CTy *t)
{
	if(isitype(t) || isptr(t))
		return ARGINT1;
	if(isstruct(t)) {
		if(t->size <= 8)
			return ARGINT1;
		if(t->size <= 16)
			return ARGINT2;
		return ARGMEM;
	}
	panic("unimplemented classify");
}

static Vec *
classifyargs(CTy *f, int returnstruct)
{
	int      i, sz, nintargs, offset;
	Vec      *locs, *params;
	Argclass c;
	Argloc   *loc;
	NameTy   *nt;
	
	params = f->Func.params;
	locs = vec();
	offset = 16;
	nintargs = 0;
	if(returnstruct)
		nintargs = 1;
	for(i = 0; i < params->len; i++) {
		nt = vecget(params, i);
		sz = nt->type->size;
		if(sz < 8)
			sz = 8;
		if(sz % 8)
			sz = sz - (sz % 8) + 8;
		loc = gcmalloc(sizeof(Argloc));
		c = classify(nt->type);
		switch(c) {
		case ARGINT1:
			if(nintargs == 6)
				goto argmem;
			goto argint1;
		case ARGINT2:
			if(nintargs >= 5)
				goto argmem;
			goto argint2;
		case ARGMEM:
			goto argmem;
		default:
			panic("internal error classify args");
		}
	argmem:
		loc->class = ARGMEM;
		loc->offset = offset;
		offset += sz;
		vecappend(locs, loc);
		continue;
	argint1:
		loc->class = ARGINT1;
		loc->r1 = nintargs++;
		vecappend(locs, loc);
		continue;
	argint2:
		loc->class = ARGINT2;
		loc->r1 = nintargs++;
		loc->r2 = nintargs++;
		vecappend(locs, loc);
		continue;
	}
	if(nintargs > 6)
		panic("internal error");
	return locs;
}

static void
calcslotoffsets(Node *f)
{
	int i, tsz, curoffset;
	StkSlot *s;

	curoffset = 0;
	for(i = 0; i < f->Func.stkslots->len; i++) {
		s = vecget(f->Func.stkslots, i);
		tsz = s->size;
		/* This allows us to just copy params which have had the size rounded up. */
		if(tsz < 8)
			tsz = 8;
		else if(tsz < 16)
			tsz = 16;
		curoffset += tsz;
		if(curoffset % s->align)
			curoffset = (curoffset - (curoffset % s->align) + s->align);
		s->offset = -curoffset;
	}
	if(curoffset % 16)
		curoffset = (curoffset - (curoffset % 16) + 16);
	f->Func.localsz = curoffset;
}

static void
pushq(char *reg)
{
	stackoffset += 8;
	out("pushq %%%s\n", reg);
}

static void
popq(char *reg)
{
	stackoffset -= 8;
	out("popq %%%s\n", reg);
}

void
calcscratcharea(Node *n)
{
	int  i;

	switch(n->t) {
	case NCALL:
		if(isstruct(n->type)) {
			if(n->type->size > scratcharea->size)
				scratcharea->size = n->type->size;
			if(n->type->align > scratcharea->align)
				scratcharea->align = n->type->align;
		}
		for(i = 0; i < n->Call.args->len; i++)
			calcscratcharea(vecget(n->Call.args, i));
		break;	
	case NFUNC:
		calcscratcharea(n->Func.body);
		break;
	case NASSIGN:
		calcscratcharea(n->Assign.l);
		calcscratcharea(n->Assign.r);
		break;
	case NBINOP:
		calcscratcharea(n->Binop.l);
		calcscratcharea(n->Binop.r);
		break;
	case NUNOP:
		calcscratcharea(n->Unop.operand);
		break;
	case NINCDEC:
		calcscratcharea(n->Incdec.operand);
		break;
	case NSEL:
		calcscratcharea(n->Sel.operand);
		break;
	case NIDX:
		calcscratcharea(n->Idx.operand);
		calcscratcharea(n->Idx.idx);
		break;
	case NCAST:
		calcscratcharea(n->Cast.operand);
		break;
	case NCOND:
		calcscratcharea(n->Cond.cond);
		calcscratcharea(n->Cond.iftrue);
		calcscratcharea(n->Cond.iffalse);
		break;
	case NCOMMA:
		for(i = 0; i < n->Comma.exprs->len; i++)
			calcscratcharea(vecget(n->Comma.exprs, i));
		break;
	case NBLOCK:
		for(i = 0; i < n->Block.stmts->len; i++)
			calcscratcharea(vecget(n->Block.stmts, i));
		break;
	case NIF:
		calcscratcharea(n->If.expr);
		calcscratcharea(n->If.iftrue);
		if(n->If.iffalse)
			calcscratcharea(n->If.iffalse);
		break;
	case NFOR:
		if(n->For.init)
			calcscratcharea(n->For.init);
		if(n->For.cond)
			calcscratcharea(n->For.cond);
		if(n->For.step)
			calcscratcharea(n->For.step);
		calcscratcharea(n->For.stmt);
		break;
	case NSWITCH:
		calcscratcharea(n->Switch.expr);
		calcscratcharea(n->Switch.stmt);
		break;
	case NDOWHILE:
		calcscratcharea(n->DoWhile.stmt);
		calcscratcharea(n->DoWhile.expr);
		break;
	case NWHILE:
		calcscratcharea(n->While.expr);
		calcscratcharea(n->While.stmt);
		break;
	case NCASE:
		calcscratcharea(n->Case.stmt);
		break;
	case NLABELED:
		calcscratcharea(n->Labeled.stmt);
		break;
	case NRETURN:
		if(n->Return.expr)
			calcscratcharea(n->Return.expr);
		break;
	case NDECL:
		/* TODO: important */
		break;
	case NEXPRSTMT:
		if(n->ExprStmt.expr)
			calcscratcharea(n->ExprStmt.expr);
	case NSIZEOF:
	case NGOTO:
	case NNUM:
	case NSTR:
	case NIDENT:
		break;
	default:
		panic("unimlpemented calcscratcharea %d", n->t);
	}
}

static void
func(Node *f)
{
	Vec      *args, *argpos;
	Sym      *sym;
	Argloc   *aloc;
	Argclass c;
	int      i;
	
	stackoffset = 0;
	scratcharea = gcmalloc(sizeof(StkSlot));
	scratcharea->size = 8;
	scratcharea->align = 8;
	vecappend(f->Func.stkslots, scratcharea);
	calcscratcharea(f);
	c = classify(f->type->Func.rtype);
	if(c == ARGMEM) {
		memretptr = gcmalloc(sizeof(StkSlot));
		memretptr->size = 8;
		memretptr->align = 8;
		vecappend(f->Func.stkslots, memretptr);
	} else {
		memretptr = 0;
	}
	calcslotoffsets(f);
	out(".text\n");
	out(".globl %s\n", f->Func.name);
	out("%s:\n", f->Func.name);
	pushq("rbp");
	out("movq %%rsp, %%rbp\n");
	if(f->Func.localsz) {
		out("sub $%d, %%rsp\n", f->Func.localsz);
		stackoffset += f->Func.localsz;
	}
	args = f->Func.params;
	argpos = classifyargs(f->type, c == ARGMEM);
	if(c == ARGMEM)
		out("movq %%rdi, %d(%%rbp)\n", memretptr->offset);
	for(i = 0; i < args->len; i++) {
		sym = vecget(args, i);
		aloc = vecget(argpos, i);
		switch(aloc->class) {
		case ARGINT2:
			out("movq %%%s, %d(%%rbp)\n", intargregs[aloc->r2], sym->Local.slot->offset + 8);
		case ARGINT1:
			out("movq %%%s, %d(%%rbp)\n", intargregs[aloc->r1], sym->Local.slot->offset);
			break;
		default:
			;
		}
	}
	for(i = 0; i < args->len; i++) {
		sym = vecget(args, i);
		aloc = vecget(argpos, i);
		switch(aloc->class) {
		case ARGMEM:
			if(isitype(sym->type)) {
				out("movq %d(%%rbp), %%rcx\n", aloc->offset);
				out("leaq %d(%%rbp), %%rax\n", sym->Local.slot->offset);
				store(sym->type);
				break;
			}
			if(isstruct(sym->type)) {
				out("leaq %d(%%rbp), %%rcx\n", aloc->offset);
				out("leaq %d(%%rbp), %%rax\n", sym->Local.slot->offset);
				store(sym->type);
				break;
			}
			panic("unimplemented arg type");
		default:
			;
		}
	}
	block(f->Func.body);
	if(c == ARGMEM)
		out("movq %d(%%rbp), %%rax\n", memretptr->offset);
	out("leave\n");
	stackoffset -= f->Func.localsz;
	stackoffset -= 8;
	out("ret\n");
	if(stackoffset != 0) {
		panic("stack not balanced.");
	}
}

static void
call(Node *n)
{
	int      i, nintargs, cleanup;
	Vec      *args, *arglocs;
	Node     *arg;
	CTy      *fty;
	Argloc   *aloc;
	Argclass rcls;

	out("# call\n");
	args = n->Call.args;
	if(isptr(n->Call.funclike->type))
		fty = n->Call.funclike->type->Ptr.subty;
	else
		fty = n->Call.funclike->type;
	rcls = classify(fty->Func.rtype);
	arglocs = classifyargs(fty, rcls == ARGMEM);
	cleanup = 0;
	i = args->len;
	/* Calculate size of mem arg area. */
	while(i-- != 0) {
		arg  = vecget(args, i);
		aloc = vecget(arglocs, i);
		if(aloc->class != ARGMEM)
			continue;
		if(arg->type->size % 8)
			cleanup += arg->type->size - (arg->type->size % 8) + 8;
		else
			cleanup += arg->type->size;
	}
	if((stackoffset + cleanup) % 8)
		panic("internal error, call stack alignment");
	/* Align stack before pushing args */
	if((stackoffset + cleanup) % 16) {
		pushq("rax");
		cleanup += 8;
	}
	/* Push mem args in reverse order */
	i = args->len;
	while(i-- != 0) {
		arg  = vecget(args, i);
		aloc = vecget(arglocs, i);
		if(aloc->class != ARGMEM)
			continue;
		if(isitype(arg->type) || isptr(arg->type)) {
			expr(arg);
			pushq("rax");
			continue;
		}
		if(isstruct(arg->type)) {
			expr(arg);
			/* Struct size rounded up to 8 alignment */
			pushstruct(arg->type);
			continue;
		}
		panic("unimlpemented artype in call");
	}
	/* Push int args in reverse order */
	i = args->len;
	nintargs = 0;
	while(i-- != 0) {
		arg  = vecget(args, i);
		aloc = vecget(arglocs, i);
		switch(aloc->class) {
		case ARGINT2:
			nintargs += 2;
			if(isstruct(arg->type)) {
				expr(arg);
				out("movq 8(%%rax), %%rcx\n");
				pushq("rcx");
				out("movq (%%rax), %%rcx\n");
				pushq("rcx");
				break;
			}
			panic("unimplemented int 2 reg");
		case ARGINT1:
			nintargs += 1;
			if(isitype(arg->type) || isptr(arg->type)) {
				expr(arg);
				pushq("rax");
				break;
			}
			if(isstruct(arg->type)) {
				expr(arg);
				out("movq (%%rax), %%rcx\n");
				pushq("rcx");
				break;
			}
			panic("unimplemented int reg");
			break;
		default:
			continue;
		}
	}
	/* Pop int args back to registers */
	for(i = 0; i < nintargs; i++) {
		out("popq %%%s\n", intargregs[i]);
		stackoffset -= 8;
	}
	if(rcls == ARGMEM)
		out("lea %d(%%rbp), %%rdi\n", scratcharea->offset);
	expr(n->Call.funclike);
	out("call *%%rax\n");
	if(cleanup) {
		out("add $%d, %%rsp\n", cleanup);
		stackoffset -= cleanup;
	}
	if(isstruct(fty->Func.rtype)) {
		if(rcls == ARGINT1) {
			out("movq %%rax, %d(%%rbp)\n", scratcharea->offset);
		} else if(rcls == ARGINT2) {
			out("movq %%rax, %d(%%rbp)\n", scratcharea->offset);
			out("movq %%rdx, %d(%%rbp)\n", scratcharea->offset + 8);
		}
		out("leaq %d(%%rbp), %%rax\n", scratcharea->offset);
	}
}

static void
load(CTy *t)
{
	if(isitype(t) || isptr(t)) {
		switch(t->size) {
		case 8:
			out("movq (%%rax), %%rax\n");
			break;
		case 4:
			out("movslq (%%rax), %%rax\n");
			break;
		case 2:
			out("movswq (%%rax), %%rax\n");
			break;
		case 1:
			out("movsbq (%%rax), %%rax\n");
			break;
		default:
			panic("internal error\n");
		}
		return;
	}
	if(isstruct(t)) {
		return;
	}
	if(isarray(t)) {
		return;
	}
	if(isfunc(t)) {
		return;
	}
	errorf("unimplemented load %d\n", t->t);
}

static void
store(CTy *t)
{
	int sz, offset;

	if(isitype(t) || isptr(t)) {
		switch(t->size) {
		case 8:
			out("movq %%rcx, (%%rax)\n");
			break;
		case 4:
			out("movl %%ecx, (%%rax)\n");
			break;
		case 2:
			out("movw %%cx, (%%rax)\n");
			break;
		case 1:
			out("movb %%cl, (%%rax)\n");
			break;
		default:
			panic("internal error\n");
		}
		return;
	}
	if(isstruct(t)) {
		sz = t->size;
		offset = 0;
		pushq("rdx");
		while(sz >= 8) {
			out("movq %d(%%rcx), %%rdx\n", offset);
			out("movq %%rdx, %d(%%rax)\n", offset);
			sz -= 8;
			offset += 8;
		}
		while(sz >= 4) {
			out("movl %d(%%rcx), %%edx\n", offset);
			out("movl %%edx, %d(%%rax)\n", offset);
			sz -= 4;
			offset += 4;
		}
		while(sz) {
			out("movb %d(%%rcx), %%dl\n", offset);
			out("movb %%dl, %d(%%rax)\n", offset);
			sz--;
			offset--;
		}
		popq("rdx");
		return;
	}
	errorf("unimplemented store\n");
}

/* Returns number of bytes pushed
   The stack is kept 8 byte aligned,
   so may push more bytes. */
static void
pushstruct(CTy *t)
{
	int sz;

	if(!isstruct(t))
		panic("internal error pushstruct");
	sz = t->size;
	if(sz % 8)
		sz = sz - (sz % 8) + 8;
	out("sub $%d, %%rsp\n", sz);
	stackoffset += sz;
	out("movq %%rax, %%rcx\n");
	out("movq %%rsp, %%rax\n");
	store(t);
}

static void
decl(Node *n)
{
	int  i;
	Sym *sym;

	for(i = 0; i < n->Decl.syms->len; i++) {
		sym = vecget(n->Decl.syms, i);
		emitsym(sym);
	}
}

static void
ereturn(Node *r)
{
	CTy *ty;
	
	ty = r->Return.expr->type;
	expr(r->Return.expr);
	if(isstruct(ty)) {
		switch(classify(ty)) {
		case ARGMEM:
			out("movq %%rax, %%rcx\n");
			out("movq %d(%%rbp), %%rax\n", memretptr->offset);
			store(ty);
			break;
		case ARGINT1:
			out("movq (%%rax), %%rax\n");
			break;
		case ARGINT2:
			out("movq 8(%%rax), %%rdx\n");
			out("movq (%%rax), %%rax\n");
			break;
		}
	}
	/* No need to cleanup with leave */
	out("leave\n");
	out("ret\n");
}


static void
addr(Node *n)
{
	int sz;
	Sym *sym;
	StructMember *sm;
	
	switch(n->t) {
	case NUNOP:
		expr(n->Unop.operand);
		break;
	case NSEL:
		expr(n->Sel.operand);
		sm = getstructmember(n->Sel.operand->type, n->Sel.name);
		if(!sm)
			panic("internal error");
		out("addq $%d, %%rax\n", sm->offset);
		break;
	case NIDENT:
		sym = n->Ident.sym;
		switch(sym->k) {
		case SYMGLOBAL:
			out("leaq %s(%%rip), %%rax\n", sym->Global.label);
			break;
		case SYMLOCAL:
			out("leaq %d(%%rbp), %%rax\n", sym->Local.slot->offset);
			break;
		default:
			panic("internal error");
		}
		break;
	case NIDX:
		expr(n->Idx.idx);
		sz = n->type->size;
		if(sz != 1) {
			out("imul $%d, %%rax\n", sz);
		}
		pushq("rax");
		expr(n->Idx.operand);
		popq("rcx");
		out("addq %%rcx, %%rax\n");
		break;
	default:
		errorf("unimplemented addr\n");
	}
}

static void
obinop(int op, CTy *t)
{
	char *lset;
	char *lafter;
	char *opc;

	switch(op) {
	case '+':
		out("addq %%rcx, %%rax\n");
		break;
	case '-':
		out("subq %%rcx, %%rax\n");
		break;
	case '*':
		out("imul %%rcx, %%rax\n");
		break;
	case '/':
		out("cqto\n");
		out("idiv %%rcx\n");
		break;
	case '%':
		out("cqto\n");
		out("idiv %%rcx\n");
		out("mov %%rdx, %%rax\n");
		break;
	case '|':
		out("or %%rcx, %%rax\n");
		break;
	case '&':
		out("and %%rcx, %%rax\n");
		break;
	case '^':
		out("xor %%rcx, %%rax\n");
		break;
	case TOKSHR:
		out("sar %%cl, %%rax\n");
		break;
	case TOKSHL:
		out("sal %%cl, %%rax\n");
		break;
	case TOKEQL:
	case TOKNEQ:
	case TOKGEQ:
	case TOKLEQ:
	case '>':
	case '<':
		lset = newlabel();
		lafter = newlabel();
		switch(op) {
		case TOKEQL:
			opc = "jz";
			break;
		case TOKNEQ:
			opc = "jnz";
			break;
		case '<':
			opc = "jl";
			break;
		case '>':
			opc = "jg";
			break;
		case TOKGEQ:
			opc = "jge";
			break;
		case TOKLEQ:
			opc = "jle";
			break;
		}
		out("cmp %%rcx, %%rax\n");
		out("%s .%s\n", opc, lset);
		out("movq $0, %%rax\n");
		out("jmp .%s\n", lafter);
		out(".%s:\n", lset);
		out("movq $1, %%rax\n");
		out(".%s:\n", lafter);
		break;
	default:
		errorf("unimplemented binop %d\n", op);
	}
}

static void
assign(Node *n)
{
	Node *l, *r;
	int op;

	op = n->Assign.op;
	l = n->Assign.l;
	r = n->Assign.r;
	if(op == '=') {
		expr(r);
		pushq("rax");
		addr(l);
		popq("rcx");
		if(!isptr(l->type) && !isitype(l->type) && !isstruct(l->type))
			errorf("unimplemented assign\n");
		store(l->type);
		out("movq %%rcx, %%rax\n");
		return;
	}
	addr(l);
	pushq("rax");
	load(l->type);
	pushq("rax");
	expr(r);
	out("movq %%rax, %%rcx\n");
	popq("rax");
	obinop(op, n->type);
	out("movq %%rax, %%rcx\n");
	popq("rax");
	store(l->type);
	out("movq %%rcx, %%rax\n");
}

static void
shortcircuit(Node *n)
{
	char *t, *f, *e;

	t = newlabel();
	f = newlabel();
	e = newlabel();

	expr(n->Binop.l);
	if(n->Binop.op == TOKLAND) {
		out("testq %%rax, %%rax\n");
		out("jz .%s\n", f);
	} else if(n->Binop.op == TOKLOR) {
		out("testq %%rax, %%rax\n");
		out("jnz .%s\n", t);
	} else {
		panic("internal error");
	}
	expr(n->Binop.r);
	if(n->Binop.op == TOKLAND) {
		out("testq %%rax, %%rax\n");
		out("jz .%s\n", f);
		out("jmp .%s\n", t);
	} else if(n->Binop.op == TOKLOR) {
		out("testq %%rax, %%rax\n");
		out("jnz .%s\n", t);
		out("jmp .%s\n", f);
	} else {
		panic("internal error");
	}
	out(".%s:\n", t);
	out("mov $1, %%rax\n");
	out("jmp .%s\n", e);
	out(".%s:\n", f);
	out("xor %%rax, %%rax\n");
	out("jmp .%s\n", e);
	out(".%s:\n", e);
}

static void
binop(Node *n)
{
	if(n->Binop.op == TOKLAND || n->Binop.op == TOKLOR) {
		shortcircuit(n);
		return;
	}
	expr(n->Binop.l);
	pushq("rax");
	expr(n->Binop.r);
	out("movq %%rax, %%rcx\n");
	popq("rax");
	obinop(n->Binop.op, n->type);
}

static void
unop(Node *n)
{
	switch(n->Unop.op) {
	case '*':
		expr(n->Unop.operand);
		load(n->type);
		break;
	case '&':
		addr(n->Unop.operand);
		break;
	case '~':
		expr(n->Unop.operand);
		out("notq %%rax\n");
		break;
	case '!':
		expr(n->Unop.operand);
		out("xorq %%rcx, %%rcx\n");
		out("testq %%rax, %%rax\n");
		out("setz %%cl\n");
		out("movq %%rcx, %%rax\n");
		break;
	case '-':
		expr(n->Unop.operand);
		out("neg %%rax\n");
		break;
	case TOKINC:
	default:
		errorf("unimplemented unop %d\n", n->Unop.op);
	}
}

static void
incdec(Node *n)
{
	if(!isitype(n->type) && !isptr(n->type))
		panic("unimplemented incdec");
	addr(n->Incdec.operand);
	pushq("rax");
	load(n->type);
	if(isptr(n->type)) {
		if(n->Incdec.op == TOKINC)
			out("add $%d, %%rax\n", n->type->Ptr.subty->size);
		else
			out("add $%d, %%rax\n", -n->type->Ptr.subty->size);
	} else {
		if(n->Incdec.op == TOKINC)
			out("inc %%rax\n");
		else
			out("dec %%rax\n");
	}
	out("movq %%rax, %%rcx\n");
	popq("rax");
	store(n->type);
	out("movq %%rcx, %%rax\n");
	if(n->Incdec.post == 1) {
		if(n->Incdec.op == TOKINC)
			out("dec %%rax\n");
		else
			out("inc %%rax\n");
	}
}

static void
ident(Node *n)
{
	Sym *sym;

	sym = n->Ident.sym;
	if(sym->k == SYMENUM) {
		out("movq $%d, %%rax\n", sym->Enum.v);
		return;
	}
	addr(n);
	load(n->type);
}

static void
eif(Node *n)
{
	char *end;

	end = newlabel();
	expr(n->If.expr);
	out("test %%rax, %%rax\n");
	out("jz .%s\n", n->If.lelse);
	stmt(n->If.iftrue);
	out("jmp .%s\n", end);
	out(".%s:\n", n->If.lelse);
	if(n->If.iffalse)
		stmt(n->If.iffalse);
	out(".%s:\n", end);
}

static void
efor(Node *n)
{
	if(n->For.init)
		expr(n->For.init);
	out(".%s:\n", n->For.lstart);
	if(n->For.cond) {
		expr(n->For.cond);
		out("test %%rax, %%rax\n");
		out("jz .%s\n", n->For.lend);
	}
	stmt(n->For.stmt);
	if(n->For.step)
		expr(n->For.step);
	out("jmp .%s\n", n->For.lstart);
	out(".%s:\n", n->For.lend);
}

static void
ewhile(Node *n)
{
	out(".%s:\n", n->While.lstart);
	expr(n->While.expr);
	out("test %%rax, %%rax\n");
	out("jz .%s\n", n->While.lend);
	stmt(n->While.stmt);
	out("jmp .%s\n", n->While.lstart);
	out(".%s:\n", n->While.lend);
}

static void
dowhile(Node *n)
{
	out(".%s:\n", n->DoWhile.lstart);
	stmt(n->DoWhile.stmt);
	out(".%s:\n", n->DoWhile.lcond);
	expr(n->DoWhile.expr);
	out("test %%rax, %%rax\n");
	out("jz .%s\n", n->DoWhile.lend);
	out("jmp .%s\n", n->DoWhile.lstart);
	out(".%s:\n", n->DoWhile.lend);
}

static void
eswitch(Node *n)
{
	int   i;
	Node *c;

	expr(n->Switch.expr);
	for(i = 0; i < n->Switch.cases->len; i++) {
		c = vecget(n->Switch.cases, i);
		out("mov $%lld, %%rcx\n", c->Case.cond);
		out("cmp %%rax, %%rcx\n");
		out("je .%s\n", c->Case.l);
	}
	if(n->Switch.ldefault) {
		out("jmp .%s\n", n->Switch.ldefault);
	} else {
		out("jmp .%s\n", n->Switch.lend);
	}
	stmt(n->Switch.stmt);
	out(".%s:\n", n->Switch.lend);
}

static void
cond(Node *n)
{
	char *lfalse, *lend;

	if(!isitype(n->type) && !isptr(n->type))
		panic("unimplemented emit cond");
	expr(n->Cond.cond);
	lfalse = newlabel();
	lend = newlabel();
	out("test %%rax, %%rax\n");
	out("jz .%s\n", lfalse);
	expr(n->Cond.iftrue);
	out("jmp .%s\n", lend);
	out(".%s:\n", lfalse);
	expr(n->Cond.iffalse);
	out(".%s:\n", lend);
}

static void
cast(Node *n)
{
	CTy *from;
	CTy *to;
	
	expr(n->Cast.operand);
	from = n->Cast.operand->type;
	to = n->type;
	if(isptr(from) && isptr(to))
		return;
	if(isptr(to) && isitype(from))
		return;
	if(isptr(from) && isitype(to))
		return;
	if(isitype(from) && isitype(to))
		return;
	if(isfunc(from) && isptr(to))
		return;
	if(isarray(from) && isptr(to))
		return;
	errorf("unimplemented cast %d %d\n", from->t, to->t);
}

static void
sel(Node *n)
{
	CTy *t;
	int offset;

	expr(n->Sel.operand);
	t = n->Sel.operand->type;
	offset = getstructmember(t, n->Sel.name)->offset;
	if(offset != 0) {
		out("add $%d, %%rax\n", offset);
	}
	load(n->type);
}

static void
idx(Node *n)
{
	int sz;

	expr(n->Idx.idx);
	sz = n->type->size;
	if(sz != 1) {
		out("imul $%d, %%rax\n", sz);
	}
	out("push %%rax\n");
	expr(n->Idx.operand);
	out("pop %%rcx\n");
	out("addq %%rcx, %%rax\n");
	load(n->type);
}

static void
comma(Node *n)
{
	int i;

	for(i = 0; i < n->Comma.exprs->len; i++) {
		expr(vecget(n->Comma.exprs, i));
	}
}

static void
str(Node *n)
{
	char *l;

	l = newlabel();
	out(".data\n");
	out(".%s:\n", l);
	out(".string %s\n", n->Str.v);
	out(".text\n");
	out("leaq .%s(%%rip), %%rax\n", l);
}

static void
expr(Node *n)
{
	switch(n->t){
	case NCOMMA:
		comma(n);
		break;
	case NCAST:
		cast(n);
		break;
	case NSTR:
		str(n);
		break;
	case NSIZEOF:
		out("movq $%lld, %%rax\n", n->Sizeof.type->size);
		break;
	case NNUM:
		out("movq $%lld, %%rax\n", n->Num.v);
		break;
	case NIDENT:
		ident(n);
		break;
	case NUNOP:
		unop(n);
		break;
	case NASSIGN:
		assign(n);
		break;
	case NBINOP:
		binop(n);
		break;
	case NIDX:
		idx(n);
		break;
	case NSEL:
		sel(n);
		break;
	case NCOND:
		cond(n);
		break;
	case NCALL:
		call(n);
		break;
	case NINCDEC:
		incdec(n);
		break;
	default:
		errorf("unimplemented emit expr %d\n", n->t);
	}
}

static void
stmt(Node *n)
{
	switch(n->t){
	case NDECL:
		decl(n);
		out(".text\n");
		break;
	case NRETURN:
		ereturn(n);
		break;
	case NIF:
		eif(n);
		break;
	case NWHILE:
		ewhile(n);
		break;
	case NFOR:
		efor(n);
		break;
	case NDOWHILE:
		dowhile(n);
		break;
	case NBLOCK:
		block(n);
		break;
	case NSWITCH:
		eswitch(n);
		break;
	case NGOTO:
		out("jmp .%s\n", n->Goto.l);
		break;
	case NCASE:
		out(".%s:\n", n->Case.l);
		stmt(n->Case.stmt);
		break;
	case NLABELED:
		out(".%s:\n", n->Labeled.l);
		stmt(n->Labeled.stmt);
		break;
	case NEXPRSTMT:
		if(n->ExprStmt.expr)
			expr(n->ExprStmt.expr);
		break;
	default:
		errorf("unimplemented emit stmt %d\n", n->t);
	}	
}

void
emitsym(Sym *sym)
{
	out("# emit sym %s\n", sym->name);
	switch(sym->k){
	case SYMGLOBAL:
		if(isfunc(sym->type)) {
			func(sym->Global.init);
			return;
		}
		out(".data\n");
		out(".comm %s, %d, %d\n", sym->name, sym->type->size, sym->type->align);
		break;
	case SYMLOCAL:
		break;
	case SYMENUM:
	case SYMTYPE:
		panic("internal error");
	}
}
