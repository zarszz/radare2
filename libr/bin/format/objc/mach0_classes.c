/* radare - LGPL - Copyright 2015-2018 - inisider */

#include "mach0_classes.h"

#define RO_META (1 << 0)
#define MAX_CLASS_NAME_LEN 256

struct MACH0_(SMethodList) {
	ut32 entsize;
	ut32 count;
	/* SMethod first;  These structures follow inline */
};

struct MACH0_(SMethod) {
	mach0_ut name;  /* SEL (32/64-bit pointer) */
	mach0_ut types; /* const char * (32/64-bit pointer) */
	mach0_ut imp;   /* IMP (32/64-bit pointer) */
};

struct MACH0_(SClass) {
	mach0_ut isa;	/* SClass* (32/64-bit pointer) */
	mach0_ut superclass; /* SClass* (32/64-bit pointer) */
	mach0_ut cache;      /* Cache (32/64-bit pointer) */
	mach0_ut vtable;     /* IMP * (32/64-bit pointer) */
	mach0_ut data;       /* SClassRoT * (32/64-bit pointer) */
};

struct MACH0_(SClassRoT) {
	ut32 flags;
	ut32 instanceStart;
	ut32 instanceSize;
#ifdef R_BIN_MACH064
	ut32 reserved;
#endif
	mach0_ut ivarLayout;     /* const uint8_t* (32/64-bit pointer) */
	mach0_ut name;		/* const char* (32/64-bit pointer) */
	mach0_ut baseMethods;    /* const SMEthodList* (32/64-bit pointer) */
	mach0_ut baseProtocols;  /* const SProtocolList* (32/64-bit pointer) */
	mach0_ut ivars;		/* const SIVarList* (32/64-bit pointer) */
	mach0_ut weakIvarLayout; /* const uint8_t * (32/64-bit pointer) */
	mach0_ut baseProperties; /* const SObjcPropertyList* (32/64-bit pointer) */
};

struct MACH0_(SProtocolList) {
	mach0_ut count; /* uintptr_t (a 32/64-bit value) */
			/* SProtocol* list[0];  These pointers follow inline */
};

struct MACH0_(SProtocol) {
	mach0_ut isa;			/* id* (32/64-bit pointer) */
	mach0_ut name;			/* const char * (32/64-bit pointer) */
	mach0_ut protocols;		/* SProtocolList* (32/64-bit pointer) */
	mach0_ut instanceMethods;	/* SMethodList* (32/64-bit pointer) */
	mach0_ut classMethods;		/* SMethodList* (32/64-bit pointer) */
	mach0_ut optionalInstanceMethods; /* SMethodList* (32/64-bit pointer) */
	mach0_ut optionalClassMethods;    /* SMethodList* (32/64-bit pointer) */
	mach0_ut instanceProperties;      /* struct SObjcPropertyList* (32/64-bit pointer) */
};

struct MACH0_(SIVarList) {
	ut32 entsize;
	ut32 count;
	/* SIVar first;  These structures follow inline */
};

struct MACH0_(SIVar) {
	mach0_ut offset; /* uintptr_t * (32/64-bit pointer) */
	mach0_ut name;   /* const char * (32/64-bit pointer) */
	mach0_ut type;   /* const char * (32/64-bit pointer) */
	ut32 alignment;
	ut32 size;
};

struct MACH0_(SObjcProperty) {
	mach0_ut name;       /* const char * (32/64-bit pointer) */
	mach0_ut attributes; /* const char * (32/64-bit pointer) */
};

struct MACH0_(SObjcPropertyList) {
	ut32 entsize;
	ut32 count;
	/* struct SObjcProperty first;  These structures follow inline */
};

static mach0_ut get_pointer(mach0_ut p, ut32 *offset, ut32 *left, RBinFile *bf);
static void copy_sym_name_with_namespace(char *class_name, char *read_name, RBinSymbol *sym);
static void get_ivar_list_t(mach0_ut p, RBinFile *bf, RBinClass *klass);
static void get_objc_property_list(mach0_ut p, RBinFile *bf, RBinClass *klass);
static void get_method_list_t(mach0_ut p, RBinFile *bf, char *class_name, RBinClass *klass, bool is_static);
static void get_protocol_list_t(mach0_ut p, RBinFile *bf, RBinClass *klass);
static void get_class_ro_t(mach0_ut p, RBinFile *bf, ut32 *is_meta_class, RBinClass *klass);
static bool is_thumb(RBinFile *bf) {
	struct MACH0_(obj_t) *bin = (struct MACH0_(obj_t) *)bf->o->bin_obj;
	if (bin->hdr.cputype == 12) {
		if (bin->hdr.cpusubtype == 9) {
			return true;
		}
	}
	return false;
}

static mach0_ut get_pointer(mach0_ut p, ut32 *offset, ut32 *left, RBinFile *bf) {
	mach0_ut r;
	mach0_ut addr;

	static RList *sctns = NULL;
	RListIter *iter = NULL;
	RBinSection *s = NULL;
	RBinObject *obj = bf ? bf->o : NULL;
	if (!obj) {
		return 0;
	}
	if (!obj->bin_obj) {
		return 0;
	}

	struct MACH0_(obj_t) *bin = (struct MACH0_(obj_t)*) obj->bin_obj;
	if (bin->va2pa) {
		return bin->va2pa (p, offset, left, bf);
	}

	if (!sctns) {
		sctns = r_bin_plugin_mach.sections (bf);
		if (!sctns) {
			// retain just for debug
			// eprintf ("there is no sections\n");
			return 0;
		}
	}

	addr = p;
	r_list_foreach (sctns, iter, s) {
		if (addr >= s->vaddr && addr < s->vaddr + s->vsize) {
			if (offset) {
				*offset = addr - s->vaddr;
			}
			if (left) {
				*left = s->vsize - (addr - s->vaddr);
			}
			r = (s->paddr - obj->boffset  + (addr - s->vaddr));
			return r;
		}
	}

	if (offset) {
		*offset = 0;
	}
	if (left) {
		*left = 0;
	}

	return 0;
}

static void copy_sym_name_with_namespace(char *class_name, char *read_name, RBinSymbol *sym) {
	if (!class_name) {
		class_name = "";
	}
	sym->classname = strdup (class_name);
	sym->name = strdup (read_name);
}

static void get_ivar_list_t(mach0_ut p, RBinFile *bf, RBinClass *klass) {
	struct MACH0_(SIVarList) il;
	struct MACH0_(SIVar) i;
	mach0_ut r;
	ut32 offset, left, j;
	char *name = NULL;
	char *type = NULL;

	int len;
	bool bigendian;
	mach0_ut ivar_offset_p, ivar_offset;
	RBinField *field = NULL;
	ut8 sivarlist[sizeof (struct MACH0_(SIVarList))] = {0};
	ut8 sivar[sizeof (struct MACH0_(SIVar))] = {0};
	ut8 offs[sizeof (mach0_ut)] = {0};

	if (!bf || !bf->o || !bf->o->bin_obj || !bf->o->info) {
		eprintf ("uncorrect RBinFile pointer\n");
		return;
	}
	bigendian = bf->o->info->big_endian;
	if (!(r = get_pointer (p, &offset, &left, bf))) {
		return;
	}
	memset (&il, '\0', sizeof (struct MACH0_(SIVarList)));
	if (r + left < r || r + sizeof (struct MACH0_(SIVarList)) < r) {
		return;
	}
	if (r > bf->size || r + left > bf->size) {
		return;
	}
	if (r + sizeof (struct MACH0_(SIVarList)) > bf->size) {
		return;
	}
	if (left < sizeof (struct MACH0_(SIVarList))) {
		if (r_buf_read_at (bf->buf, r, sivarlist, left) != left) {
			return;
		}
	} else {
		len = r_buf_read_at (bf->buf, r, sivarlist, sizeof (struct MACH0_(SIVarList)));
		if (len != sizeof (struct MACH0_(SIVarList))) {
			return;
		}
	}
	il.entsize = r_read_ble (&sivarlist[0], bigendian, 32);
	il.count = r_read_ble (&sivarlist[4], bigendian, 32);
	p += sizeof (struct MACH0_(SIVarList));
	offset += sizeof (struct MACH0_(SIVarList));
	for (j = 0; j < il.count; j++) {
		r = get_pointer (p, &offset, &left, bf);
		if (!r) {
			return;
		}
		if (!(field = R_NEW0 (RBinField))) {
			// retain just for debug
			// eprintf ("RBinField allocation error\n");
			return;
		}
		memset (&i, '\0', sizeof (struct MACH0_(SIVar)));
		if (r + left < r || r + sizeof (struct MACH0_(SIVar)) < r) {
			goto error;
		}
		if (r > bf->size || r + left > bf->size) {
			goto error;
		}
		if (r + sizeof (struct MACH0_(SIVar)) > bf->size) {
			goto error;
		}
		if (left < sizeof (struct MACH0_(SIVar))) {
			if (r_buf_read_at (bf->buf, r, sivar, left) != left) {
				goto error;
			}
		} else {
			len = r_buf_read_at (bf->buf, r, sivar, sizeof (struct MACH0_(SIVar)));
			if (len != sizeof (struct MACH0_(SIVar))) {
				goto error;
			}
		}
		switch (sizeof (mach0_ut)) {
		case 4:
			i.offset = r_read_ble (&sivar[0], bigendian, 32);
			i.name = r_read_ble (&sivar[4], bigendian, 32);
			i.type = r_read_ble (&sivar[8], bigendian, 32);
			i.alignment = r_read_ble (&sivar[12], bigendian, 32);
			i.size = r_read_ble (&sivar[16], bigendian, 32);
			break;
		case 8:
			i.offset = r_read_ble (&sivar[0], bigendian, 64);
			i.name = r_read_ble (&sivar[8], bigendian, 64);
			i.type = r_read_ble (&sivar[16], bigendian, 64);
			i.alignment = r_read_ble (&sivar[24], bigendian, 32);
			i.size = r_read_ble (&sivar[28], bigendian, 32);
			break;
		}


		ivar_offset_p = get_pointer (i.offset, NULL, &left, bf);

		if (ivar_offset_p > bf->size) {
			goto error;
		}
		if (ivar_offset_p + sizeof (ivar_offset) > bf->size) {
			goto error;
		}
		if (ivar_offset_p != 0 && left >= sizeof (mach0_ut)) {
			len = r_buf_read_at (bf->buf, ivar_offset_p, offs, sizeof (mach0_ut));
			if (len != sizeof (mach0_ut)) {
				eprintf ("Error reading\n");
				goto error;
			}
			ivar_offset = r_read_ble (offs, bigendian, 8 * sizeof (mach0_ut));
			field->vaddr = ivar_offset;
		}

		r = get_pointer (i.name, NULL, &left, bf);
		if (r != 0) {
			struct MACH0_(obj_t) *bin = (struct MACH0_(obj_t) *)bf->o->bin_obj;
			if (r + left < r) {
				goto error;
			}
			if (r > bf->size || r + left > bf->size) {
				goto error;
			}
			if (bin->has_crypto) {
				name = strdup ("some_encrypted_data");
				left = strlen (name) + 1;
			} else {
				int name_len = R_MIN (MAX_CLASS_NAME_LEN, left);
				name = malloc (name_len + 1);
				len = r_buf_read_at (bf->buf, r, (ut8 *)name, name_len);
				if (len < 1) {
					eprintf ("Error reading\n");
					R_FREE (name);
					goto error;
				}
				name[name_len] = 0;
			}
			field->name = r_str_newf ("%s::%s%s", klass->name, "(ivar)", name);
			R_FREE (name);
		}

		r = get_pointer (i.type, NULL, &left, bf);
		if (r != 0) {
			struct MACH0_(obj_t) *bin = (struct MACH0_(obj_t) *) bf->o->bin_obj;
			int is_crypted = bin->has_crypto;
			if (r + left < r) {
				goto error;
			}
			if (r > bf->size || r + left > bf->size) {
				goto error;
			}
			if (is_crypted == 1) {
				type = strdup ("some_encrypted_data");
			// 	left = strlen (name) + 1;
			} else {
				int type_len = R_MIN (MAX_CLASS_NAME_LEN, left);
				type = calloc (1, type_len + 1);
				r_buf_read_at (bf->buf, r, (ut8 *)type, type_len);
				type[type_len] = 0;
			}
      			field->type = strdup (type);
			R_FREE (type);
		}

		r_list_append (klass->fields, field);
		p += sizeof (struct MACH0_(SIVar));
		offset += sizeof (struct MACH0_(SIVar));
	}
	return;

error:
	r_bin_field_free (field);
}

///////////////////////////////////////////////////////////////////////////////
static void get_objc_property_list(mach0_ut p, RBinFile *bf, RBinClass *klass) {
	struct MACH0_(SObjcPropertyList) opl;
	struct MACH0_(SObjcProperty) op;
	mach0_ut r;
	ut32 offset, left, j;
	char *name = NULL;
	int len;
	bool bigendian;
	RBinField *property = NULL;
	ut8 sopl[sizeof (struct MACH0_(SObjcPropertyList))] = {0};
	ut8 sop[sizeof (struct MACH0_(SObjcProperty))] = {0};

	if (!bf || !bf->o || !bf->o->bin_obj || !bf->o->info) {
		eprintf ("uncorrect RBinFile pointer\n");
		return;
	}
	bigendian = bf->o->info->big_endian;
	r = get_pointer (p, &offset, &left, bf);
	if (!r) {
		return;
	}
	memset (&opl, '\0', sizeof (struct MACH0_(SObjcPropertyList)));
	if (r + left < r || r + sizeof (struct MACH0_(SObjcPropertyList)) < r) {
		return;
	}
	if (r > bf->size || r + left > bf->size) {
		return;
	}
	if (r + sizeof (struct MACH0_(SObjcPropertyList)) > bf->size) {
		return;
	}
	if (left < sizeof (struct MACH0_(SObjcPropertyList))) {
		if (r_buf_read_at (bf->buf, r, sopl, left) != left) {
			return;
		}
	} else {
		len = r_buf_read_at (bf->buf, r, sopl, sizeof (struct MACH0_(SObjcPropertyList)));
		if (len != sizeof (struct MACH0_(SObjcPropertyList))) {
			return;
		}
	}

	opl.entsize = r_read_ble (&sopl[0], bigendian, 32);
	opl.count = r_read_ble (&sopl[4], bigendian, 32);

	p += sizeof (struct MACH0_(SObjcPropertyList));
	offset += sizeof (struct MACH0_(SObjcPropertyList));
	for (j = 0; j < opl.count; j++) {
		r = get_pointer (p, &offset, &left, bf);
		if (!r) {
			return;
		}

		if (!(property = R_NEW0 (RBinField))) {
			// retain just for debug
			// eprintf("RBinClass allocation error\n");
			return;
		}

		memset (&op, '\0', sizeof (struct MACH0_(SObjcProperty)));

		if (r + left < r || r + sizeof (struct MACH0_(SObjcProperty)) < r) {
			goto error;
		}
		if (r > bf->size || r + left > bf->size) {
			goto error;
		}
		if (r + sizeof (struct MACH0_(SObjcProperty)) > bf->size) {
			goto error;
		}

		if (left < sizeof (struct MACH0_(SObjcProperty))) {
			if (r_buf_read_at (bf->buf, r, sop, left) != left) {
				goto error;
			}
		} else {
			len = r_buf_read_at (bf->buf, r, sop, sizeof (struct MACH0_(SObjcProperty)));
			if (len != sizeof (struct MACH0_(SObjcProperty))) {
				goto error;
			}
		}
		op.name = r_read_ble (&sop[0], bigendian, 8 * sizeof (mach0_ut));
		op.attributes = r_read_ble (&sop[sizeof (mach0_ut)], bigendian, 8 * sizeof (mach0_ut));
		r = get_pointer (op.name, NULL, &left, bf);
		if (r) {
			struct MACH0_(obj_t) *bin = (struct MACH0_(obj_t) *)bf->o->bin_obj;
			if (r > bf->size || r + left > bf->size) {
				goto error;
			}
			if (r + left < r) {
				goto error;
			}
			if (bin->has_crypto) {
				name = strdup ("some_encrypted_data");
				left = strlen (name) + 1;
			} else {
				int name_len = R_MIN (MAX_CLASS_NAME_LEN, left);
				name = calloc (1, name_len + 1);
				if (!name) {
					goto error;
				}
				if (r_buf_read_at (bf->buf, r, (ut8 *)name, name_len) != name_len) {
					goto error;
				}
			}
			property->name = r_str_newf ("%s::%s%s", klass->name,
						"(property)", name);
			R_FREE (name);
		}
#if 0
		r = get_pointer (op.attributes, NULL, &left, bf);
		if (r != 0) {
			struct MACH0_(obj_t) *bin = (struct MACH0_(obj_t) *) bf->o->bin_obj;
			int is_crypted = bin->has_crypto;

			if (r > bf->size || r + left > bf->size) goto error;
			if (r + left < r) goto error;

			if (is_crypted == 1) {
				name = strdup ("some_encrypted_data");
				left = strlen (name) + 1;
			} else {
				name = malloc (left);
				len = r_buf_read_at (bf->buf, r, (ut8 *)name, left);
				if (len == 0 || len == -1) goto error;
			}

			R_FREE (name);
		}
#endif
		r_list_append (klass->fields, property);

		p += sizeof (struct MACH0_(SObjcProperty));
		offset += sizeof (struct MACH0_(SObjcProperty));
	}
	return;
error:
	R_FREE (property);
	R_FREE (name);
	return;
}

///////////////////////////////////////////////////////////////////////////////
static void get_method_list_t(mach0_ut p, RBinFile *bf, char *class_name, RBinClass *klass, bool is_static) {
	struct MACH0_(SMethodList) ml;
	struct MACH0_(SMethod) m;
	mach0_ut r;
	ut32 offset, left, i;
	char *name = NULL;
  	char *rtype = NULL;
	int len;
	bool bigendian;
	ut8 sml[sizeof (struct MACH0_(SMethodList))] = {0};
	ut8 sm[sizeof (struct MACH0_(SMethod))] = {0};

	RBinSymbol *method = NULL;
	if (!bf || !bf->o || !bf->o->bin_obj || !bf->o->info) {
		eprintf ("incorrect RBinFile pointer\n");
		return;
	}
	bigendian = bf->o->info->big_endian;
	r = get_pointer (p, &offset, &left, bf);
	if (!r) {
		return;
	}
	memset (&ml, '\0', sizeof (struct MACH0_(SMethodList)));

	if (r + left < r || r + sizeof (struct MACH0_(SMethodList)) < r) {
		return;
	}
	if (r > bf->size || r + left > bf->size) {
		return;
	}
	if (r + sizeof (struct MACH0_(SMethodList)) > bf->size) {
		return;
	}
	if (left < sizeof (struct MACH0_(SMethodList))) {
		if (r_buf_read_at (bf->buf, r, sml, left) != left) {
			return;
		}
	} else {
		len = r_buf_read_at (bf->buf, r, sml, sizeof (struct MACH0_(SMethodList)));
		if (len != sizeof (struct MACH0_(SMethodList))) {
			return;
		}
	}
	ml.entsize = r_read_ble (&sml[0], bigendian, 32);
	ml.count = r_read_ble (&sml[4], bigendian, 32);

	p += sizeof (struct MACH0_(SMethodList));
	offset += sizeof (struct MACH0_(SMethodList));
	for (i = 0; i < ml.count; i++) {
		r = get_pointer (p, &offset, &left, bf);
		if (!r) {
			return;
		}

		if (!(method = R_NEW0 (RBinSymbol))) {
			// retain just for debug
			// eprintf ("RBinClass allocation error\n");
			return;
		}
		memset (&m, '\0', sizeof (struct MACH0_(SMethod)));
		if (r + left < r || r + sizeof (struct MACH0_(SMethod)) < r) {
			goto error;
		}
		if (r > bf->size || r + left > bf->size) {
			goto error;
		}
		if (r + sizeof (struct MACH0_(SMethod)) > bf->size) {
			goto error;
		}
		if (left < sizeof (struct MACH0_(SMethod))) {
			if (r_buf_read_at (bf->buf, r, sm, left) != left) {
				goto error;
			}
		} else {
			len = r_buf_read_at (bf->buf, r, sm, sizeof (struct MACH0_(SMethod)));
			if (len != sizeof (struct MACH0_(SMethod))) {
				goto error;
			}
		}
		m.name = r_read_ble (&sm[0], bigendian, 8 * sizeof (mach0_ut));
		m.types = r_read_ble (&sm[sizeof (mach0_ut)], bigendian, 8 * sizeof (mach0_ut));
		m.imp = r_read_ble (&sm[2 * sizeof (mach0_ut)], bigendian, 8 * sizeof (mach0_ut));

		r = get_pointer (m.name, NULL, &left, bf);
		if (r) {
			struct MACH0_(obj_t) *bin = (struct MACH0_(obj_t) *)bf->o->bin_obj;
			if (r + left < r) {
				goto error;
			}
			if (r > bf->size || r + left > bf->size) {
				goto error;
			}
			if (bin->has_crypto) {
				name = strdup ("some_encrypted_data");
				left = strlen (name) + 1;
			} else {
				int name_len = R_MIN (MAX_CLASS_NAME_LEN, left);
				name = malloc (name_len + 1);
				len = r_buf_read_at (bf->buf, r, (ut8 *)name, name_len);
				name[name_len] = 0;
				if (len < 1) {
					goto error;
				}
			}
			copy_sym_name_with_namespace (class_name, name, method);
			R_FREE (name);
		}

		r = get_pointer (m.types, NULL, &left, bf);
		if (r != 0) {
			struct MACH0_(obj_t) *bin = (struct MACH0_(obj_t) *)bf->o->bin_obj;

			if (r + left < r || r > bf->size || r + left > bf->size) {
				goto error;
			}
			if (bin->has_crypto) {
				rtype = strdup ("some_encrypted_data");
				left = strlen (rtype) + 1;
			} else {
				left = 1;
				rtype = malloc (left + 1);
				if (!rtype) {
					goto error;
				}
				if (r_buf_read_at (bf->buf, r, (ut8 *)rtype, left) != left) {
					free (rtype);
					goto error;
				}
				rtype[left] = 0;
			}
      			method->rtype = strdup (rtype);
			R_FREE (rtype);
		}

		method->vaddr = m.imp;
		method->type = is_static ? R_BIN_TYPE_FUNC_STR : "METH";
		if (is_static) {
			method->method_flags |= R_BIN_METH_CLASS;
		}
		if (is_thumb (bf)) {
			if (method->vaddr & 1) {
				method->vaddr >>= 1;
				method->vaddr <<= 1;
				//eprintf ("0x%08llx METHOD %s\n", method->vaddr, method->name);
			}
		}
		r_list_append (klass->methods, method);
		p += sizeof (struct MACH0_(SMethod));
		offset += sizeof (struct MACH0_(SMethod));
	}
	return;
error:
	R_FREE (method);
	R_FREE (name);
	return;
}

///////////////////////////////////////////////////////////////////////////////
static void get_protocol_list_t(mach0_ut p, RBinFile *bf, RBinClass *klass) {
	struct MACH0_(SProtocolList) pl = { 0 };
	struct MACH0_(SProtocol) pc;
	char *class_name = NULL;
	ut32 offset, left, i, j;
	mach0_ut q, r;
	int len;
	bool bigendian;
	ut8 spl[sizeof (struct MACH0_(SProtocolList))] = {0};
	ut8 spc[sizeof (struct MACH0_(SProtocol))] = {0};
	ut8 sptr[sizeof (mach0_ut)] = {0};

	if (!bf || !bf->o || !bf->o->bin_obj || !bf->o->info) {
		eprintf ("get_protocol_list_t: Invalid RBinFile pointer\n");
		return;
	}
	bigendian = bf->o->info->big_endian;
	if (!(r = get_pointer (p, &offset, &left, bf))) {
		return;
	}
	if (r + left < r || r + sizeof (struct MACH0_(SProtocolList)) < r) {
		return;
	}
	if (r > bf->size || r + left > bf->size) {
		return;
	}
	if (r + sizeof (struct MACH0_(SProtocolList)) > bf->size) {
		return;
	}
	if (left < sizeof (struct MACH0_(SProtocolList))) {
		if (r_buf_read_at (bf->buf, r, spl, left) != left) {
			return;
		}
	} else {
		len = r_buf_read_at (bf->buf, r, spl, sizeof (struct MACH0_(SProtocolList)));
		if (len != sizeof (struct MACH0_(SProtocolList))) {
			return;
		}
	}
	pl.count = r_read_ble (&spl[0], bigendian, 8 * sizeof (mach0_ut));

	p += sizeof (struct MACH0_(SProtocolList));
	offset += sizeof (struct MACH0_(SProtocolList));
	for (i = 0; i < pl.count; i++) {
		if (!(r = get_pointer (p, &offset, &left, bf))) {
			return;
		}
		if (r + left < r || r + sizeof (mach0_ut) < r) {
			return;
		}
		if (r > bf->size || r + left > bf->size) {
			return;
		}
		if (r + sizeof (mach0_ut) > bf->size) {
			return;
		}
		if (left < sizeof (ut32)) {
			if (r_buf_read_at (bf->buf, r, sptr, left) != left) {
				return;
			}
		} else {
			len = r_buf_read_at (bf->buf, r, sptr, sizeof (mach0_ut));
			if (len != sizeof (mach0_ut)) {
				return;
			}
		}
		q = r_read_ble (&sptr[0], bigendian, 8 * sizeof (mach0_ut));
		if (!(r = get_pointer (q, &offset, &left, bf))) {
			return;
		}
		memset (&pc, '\0', sizeof (struct MACH0_(SProtocol)));
		if (r + left < r || r + sizeof (struct MACH0_(SProtocol)) < r) {
			return;
		}
		if (r > bf->size || r + left > bf->size) {
			return;
		}
		if (r + sizeof (struct MACH0_(SProtocol)) > bf->size) {
			return;
		}
		if (left < sizeof (struct MACH0_(SProtocol))) {
			if (r_buf_read_at (bf->buf, r, spc, left) != left) {
				return;
			}
		} else {
			len = r_buf_read_at (bf->buf, r, spc, sizeof (struct MACH0_(SProtocol)));
			if (len != sizeof (struct MACH0_(SProtocol))) {
				return;
			}
		}
		j = 0;
		pc.isa = r_read_ble (&spc[j], bigendian, 8 * sizeof (mach0_ut));

		j += sizeof (mach0_ut);
		pc.name = r_read_ble (&spc[j], bigendian, 8 * sizeof (mach0_ut));
		j += sizeof (mach0_ut);
		pc.protocols = r_read_ble (&spc[j], bigendian, 8 * sizeof (mach0_ut));
		j += sizeof (mach0_ut);
		pc.instanceMethods = r_read_ble (&spc[j], bigendian, 8 * sizeof (mach0_ut));
		j += sizeof (mach0_ut);
		pc.classMethods = r_read_ble (&spc[j], bigendian, 8 * sizeof (mach0_ut));
		j += sizeof (mach0_ut);
		pc.optionalInstanceMethods = r_read_ble (&spc[j], bigendian, 8 * sizeof (mach0_ut));
		j += sizeof (mach0_ut);
		pc.optionalClassMethods = r_read_ble (&spc[j], bigendian, 8 * sizeof (mach0_ut));
		j += sizeof (mach0_ut);
		pc.instanceProperties = r_read_ble (&spc[j], bigendian, 8 * sizeof (mach0_ut));
		r = get_pointer (pc.name, NULL, &left, bf);
		if (r != 0) {
			char *name = NULL;
			struct MACH0_(obj_t) *bin = (struct MACH0_(obj_t) *)bf->o->bin_obj;
			if (r + left < r) {
				return;
			}
			if (r > bf->size || r + left > bf->size) {
				return;
			}
			if (bin->has_crypto) {
				name = strdup ("some_encrypted_data");
				left = strlen (name) + 1;
			} else {
				int name_len = R_MIN (MAX_CLASS_NAME_LEN, left);
				name = malloc (name_len + 1);
				if (!name) {
					return;
				}
				if (r_buf_read_at (bf->buf, r, (ut8 *)name, name_len) != name_len) {
					R_FREE (name);
					return;
				}
				name[name_len] = 0;
			}
			class_name = r_str_newf ("%s::%s%s", klass->name, "(protocol)", name);
			R_FREE (name);
		}

		if (pc.instanceMethods > 0) {
			get_method_list_t (pc.instanceMethods, bf, class_name, klass, false);
		}
		if (pc.classMethods > 0) {
			get_method_list_t (pc.classMethods, bf, class_name, klass, true);
		}
		R_FREE (class_name);
		p += sizeof (ut32);
		offset += sizeof (ut32);
	}
}

static const char *skipnum(const char *s) {
	while (IS_DIGIT (*s)) {
		s++;
	}
	return s;
}

// TODO: split up between module + classname
static char *demangle_classname(const char *s) {
	int modlen, len;
	const char *kstr;
	char *ret, *klass, *module;
	if (!strncmp (s, "_TtC", 4)) {
		len = atoi (s + 4);
		modlen = strlen (s + 4);
		if (len >= modlen) {
			return strdup (s);
		}
		module = r_str_ndup (skipnum (s + 4), len);
		kstr = skipnum (s + 4) + len;
		len = atoi (kstr);
		modlen = strlen (kstr);
		if (len >= modlen) {
			free (module);
			return strdup (s);
		}
		klass = r_str_ndup (skipnum (kstr), len);
		ret = r_str_newf ("%s.%s", module, klass);
		free (module);
		free (klass);
	} else {
		ret = strdup (s);
	}
	return ret;
}

///////////////////////////////////////////////////////////////////////////////
static void get_class_ro_t(mach0_ut p, RBinFile *bf, ut32 *is_meta_class, RBinClass *klass) {
	struct MACH0_(obj_t) * bin;
	struct MACH0_(SClassRoT) cro = { 0 };
	ut32 offset, left, i;
	ut64 r, s;
	int len;
	bool bigendian;
	ut8 scro[sizeof (struct MACH0_(SClassRoT))] = {0};

	if (!bf || !bf->o || !bf->o->bin_obj || !bf->o->info) {
		eprintf ("Invalid RBinFile pointer\n");
		return;
	}
	bigendian = bf->o->info->big_endian;
	bin = (struct MACH0_(obj_t) *)bf->o->bin_obj;
	if (!(r = get_pointer (p, &offset, &left, bf))) {
		// eprintf ("No pointer\n");
		return;
	}

	if (r + left < r || r + sizeof (cro) < r) {
		return;
	}
	if (r > bf->size || r + left > bf->size) {
		return;
	}
	if (r + sizeof (cro) > bf->size) {
		return;
	}

	// TODO: use r_buf_fread to avoid endianness issues
	if (left < sizeof (cro)) {
		eprintf ("Not enough data for SClassRoT\n");
		return;
	}
	len = r_buf_read_at (bf->buf, r, scro, sizeof (cro));
	if (len < 1) {
		return;
	}
	i = 0;
	cro.flags = r_read_ble (&scro[i], bigendian, 8 * sizeof (ut32));
	i += sizeof (ut32);
	cro.instanceStart = r_read_ble (&scro[i], bigendian, 8 * sizeof (ut32));
	i += sizeof (ut32);
	cro.instanceSize = r_read_ble (&scro[i], bigendian, 8 * sizeof (ut32));
	i += sizeof (ut32);
#ifdef R_BIN_MACH064
	cro.reserved = r_read_ble (&scro[i], bigendian, 8 * sizeof (ut32));
	i += sizeof (ut32);
#endif
	cro.ivarLayout = r_read_ble (&scro[i], bigendian, 8 * sizeof (mach0_ut));
	i += sizeof (mach0_ut);
	cro.name = r_read_ble (&scro[i], bigendian, 8 * sizeof (mach0_ut));
	i += sizeof (mach0_ut);
	cro.baseMethods = r_read_ble (&scro[i], bigendian, 8 * sizeof (mach0_ut));
	i += sizeof (mach0_ut);
	cro.baseProtocols = r_read_ble (&scro[i], bigendian, 8 * sizeof (mach0_ut));
	i += sizeof (mach0_ut);
	cro.ivars = r_read_ble (&scro[i], bigendian, 8 * sizeof (mach0_ut));
	i += sizeof (mach0_ut);
	cro.weakIvarLayout = r_read_ble (&scro[i], bigendian, 8 * sizeof (mach0_ut));
	i += sizeof (mach0_ut);
	cro.baseProperties = r_read_ble (&scro[i], bigendian, 8 * sizeof (mach0_ut));

	s = r;
	if ((r = get_pointer (cro.name, NULL, &left, bf))) {
		if (left < 1 || r + left < r) {
			return;
		}
		if (r > bf->size || r + left > bf->size) {
			return;
		}
		if (bin->has_crypto) {
			klass->name = strdup ("some_encrypted_data");
			left = strlen (klass->name) + 1;
		} else {
			int name_len = R_MIN (MAX_CLASS_NAME_LEN, left);
			char *name = malloc (name_len + 1);
			if (name) {
				int rc = r_buf_read_at (bf->buf, r, (ut8 *)name, name_len);
				if (rc != name_len) {
					rc = 0;
				}
				name[rc] = 0;
				klass->name = demangle_classname (name);
				free (name);
			}
		}
		//eprintf ("0x%x  %s\n", s, klass->name);
		sdb_num_set (bin->kv, sdb_fmt ("objc_class_%s.offset", klass->name), s, 0);
	}
#ifdef R_BIN_MACH064
	sdb_set (bin->kv, sdb_fmt ("objc_class.format", 0), "lllll isa super cache vtable data", 0);
#else
	sdb_set (bin->kv, sdb_fmt ("objc_class.format", 0), "xxxxx isa super cache vtable data", 0);
#endif

	if (cro.baseMethods > 0) {
		get_method_list_t (cro.baseMethods, bf, klass->name, klass, (cro.flags & RO_META) ? true : false);
	}

	if (cro.baseProtocols > 0) {
		get_protocol_list_t (cro.baseProtocols, bf, klass);
	}

	if (cro.ivars > 0) {
		get_ivar_list_t (cro.ivars, bf, klass);
	}

	if (cro.baseProperties > 0) {
		get_objc_property_list (cro.baseProperties, bf, klass);
	}

	if (is_meta_class) {
		*is_meta_class = (cro.flags & RO_META)? 1: 0;
	}
}

static mach0_ut get_isa_value() {
	// TODO: according to otool sources this is taken from relocs
	return 0;
}

void MACH0_(get_class_t)(mach0_ut p, RBinFile *bf, RBinClass *klass, bool dupe) {
	struct MACH0_(SClass) c = { 0 };
	const int size = sizeof (struct MACH0_(SClass));
	mach0_ut r = 0;
	ut32 offset = 0, left = 0;
	ut32 is_meta_class = 0;
	int len;
	bool bigendian;
	ut8 sc[sizeof (struct MACH0_(SClass))] = {0};
	ut32 i;

	if (!bf || !bf->o || !bf->o->info) {
		return;
	}
	bigendian = bf->o->info->big_endian;
	if (!(r = get_pointer (p, &offset, &left, bf))) {
		return;
	}
	if ((r + left) < r || (r + size) < r) {
		return;
	}
	if (r > bf->size || r + left > bf->size) {
		return;
	}
	if (r + size > bf->size) {
		return;
	}
	if (left < size) {
		eprintf ("Cannot parse obj class info out of bounds\n");
		return;
	}
	len = r_buf_read_at (bf->buf, r, sc, size);
	if (len != size) {
		return;
	}

	i = 0;
	c.isa = r_read_ble (&sc[i], bigendian, 8 * sizeof (mach0_ut));
	i += sizeof (mach0_ut);
	c.superclass = r_read_ble (&sc[i], bigendian, 8 * sizeof (mach0_ut));
	i += sizeof (mach0_ut);
	c.cache = r_read_ble (&sc[i], bigendian, 8 * sizeof (mach0_ut));
	i += sizeof (mach0_ut);
	c.vtable = r_read_ble (&sc[i], bigendian, 8 * sizeof (mach0_ut));
	i += sizeof (mach0_ut);
	c.data = r_read_ble (&sc[i], bigendian, 8 * sizeof (mach0_ut));

	klass->addr = c.isa;
	get_class_ro_t (c.data & ~0x3, bf, &is_meta_class, klass);

#if SWIFT_SUPPORT
	if (q (c.data + n_value) & 7) {
		eprintf ("This is a Swift class");
	}
#endif
	if (!is_meta_class && !dupe) {
		mach0_ut isa_n_value = get_isa_value ();
		ut64 tmp = klass->addr;
		MACH0_(get_class_t) (c.isa + isa_n_value, bf, klass, true);
		klass->addr = tmp;
	}
}

#if 0
static RList *parse_swift_classes(RBinFile *bf) {
	bool is_swift = false;
	RBinString *str;
	RListIter *iter;
	RBinClass *cls;
	RList *ret;
	char *lib;

	r_list_foreach (bf->o->libs, iter, lib) {
		if (strstr (lib, "libswift")) {
			is_swift = true;
			break;
		}
	}
	if (!is_swift) {
		return NULL;
	}

	int idx = 0;
	ret = r_list_newf (r_bin_string_free);
	r_list_foreach (bf->o->strings, iter, str) {
		if (!strncmp (str->string, "_TtC", 4)) {
			char *msg = strdup (str->string + 4);
			cls = R_NEW0 (RBinClass);
			cls->name = strdup (msg);
			cls->super = strdup (msg);
			cls->index = idx++;
			r_list_append (ret, cls);
			free (msg);
		}
	}
	return ret;
}
#endif

RList *MACH0_(parse_classes)(RBinFile *bf) {
	RList /*<RBinClass>*/ *ret = NULL;
	ut64 num_of_unnamed_class = 0;
	RBinClass *klass = NULL;
	RBinObject *obj = bf ? bf->o : NULL;
	ut32 i = 0, size = 0;
	RList *sctns = NULL;
	bool is_found = false;
	mach0_ut p = 0;
	ut32 left = 0;
	int len;
	ut64 paddr;
	ut64 s_size;
	bool bigendian;
	ut8 pp[sizeof (mach0_ut)] = {0};

	if (!bf || !obj || !obj->bin_obj || !obj->info) {
		return NULL;
	}
	bigendian = obj->info->big_endian;

	/* check if it's Swift */
	// ret = parse_swift_classes (bf);

	// sebfing of section with name __objc_classlist

	struct section_t *sections = NULL;
	if (!(sections = MACH0_(get_sections) (obj->bin_obj))) {
		return ret;
	}

	for (i = 0; !sections[i].last; i++) {
		if (strstr (sections[i].name, "__objc_classlist")) {
			is_found = true;
			paddr = sections[i].offset;
			s_size = sections[i].size;
			break;
		}
	}

	R_FREE (sections);

	if (!is_found) {
		// retain just for debug
		// eprintf ("there is no section __objc_classlist\n");
		goto get_classes_error;
	}
	// end of seaching of section with name __objc_classlist

	if (!ret && !(ret = r_list_newf ((RListFree)r_bin_class_free))) {
		// retain just for debug
		// eprintf ("RList<RBinClass> allocation error\n");
		goto get_classes_error;
	}
	// start of getting information about each class in file
	for (i = 0; i < s_size; i += sizeof (mach0_ut)) {
		left = s_size - i;
		if (left < sizeof (mach0_ut)) {
			eprintf ("Chopped classlist data\n");
			break;
		}
		if (!(klass = R_NEW0 (RBinClass))) {
			// retain just for debug
			// eprintf ("RBinClass allocation error\n");
			goto get_classes_error;
		}
		if (!(klass->methods = r_list_new ())) {
			// retain just for debug
			// eprintf ("RList<RBinField> allocation error\n");
			goto get_classes_error;
		}
		if (!(klass->fields = r_list_new ())) {
			// retain just for debug
			// eprintf ("RList<RBinSymbol> allocation error\n");
			goto get_classes_error;
		}
		size = sizeof (mach0_ut);
		if (paddr > bf->size || paddr + size > bf->size) {
			goto get_classes_error;
		}
		if (paddr + size < paddr) {
			goto get_classes_error;
		}
		len = r_buf_read_at (bf->buf, paddr + i, pp, sizeof (mach0_ut));
		if (len != sizeof (mach0_ut)) {
			goto get_classes_error;
		}
		p = r_read_ble (&pp[0], bigendian, 8 * sizeof (mach0_ut));
		MACH0_(get_class_t) (p, bf, klass, false);
		if (!klass->name) {
			klass->name = r_str_newf ("UnnamedClass%" PFMT64d, num_of_unnamed_class);
			if (!klass->name) {
				goto get_classes_error;
			}
			num_of_unnamed_class++;
		}
		r_list_append (ret, klass);
	}
	return ret;

get_classes_error:
	r_list_free (sctns);
	r_list_free (ret);
	// XXX DOUBLE FREE r_bin_class_free (klass);
	return NULL;
}
