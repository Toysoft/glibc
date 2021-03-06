/* Manage TLS descriptors.  AArch64 version.

   Copyright (C) 2011-2017 Free Software Foundation, Inc.

   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <http://www.gnu.org/licenses/>.  */

#include <link.h>
#include <ldsodefs.h>
#include <elf/dynamic-link.h>
#include <tls.h>
#include <dl-tlsdesc.h>
#include <dl-unmap-segments.h>
#include <tlsdeschtab.h>
#include <atomic.h>

/* The following functions take an entry_check_offset argument.  It's
   computed by the caller as an offset between its entry point and the
   call site, such that by adding the built-in return address that is
   implicitly passed to the function with this offset, we can easily
   obtain the caller's entry point to compare with the entry point
   given in the TLS descriptor.  If it's changed, we want to return
   immediately.  */

/* This function is used to lazily resolve TLS_DESC RELA relocations.
   The argument location is used to hold a pointer to the relocation.  */

void
attribute_hidden
_dl_tlsdesc_resolve_rela_fixup (struct tlsdesc *td, struct link_map *l)
{
  const ElfW(Rela) *reloc = atomic_load_relaxed (&td->arg);

  /* After GL(dl_load_lock) is grabbed only one caller can see td->entry in
     initial state in _dl_tlsdesc_resolve_early_return_p, other concurrent
     callers will return and retry calling td->entry.  The updated td->entry
     synchronizes with the single writer so all read accesses here can use
     relaxed order.  */
  if (_dl_tlsdesc_resolve_early_return_p
      (td, (void*)(D_PTR (l, l_info[ADDRIDX (DT_TLSDESC_PLT)]) + l->l_addr)))
    return;

  /* The code below was borrowed from _dl_fixup(),
     except for checking for STB_LOCAL.  */
  const ElfW(Sym) *const symtab
    = (const void *) D_PTR (l, l_info[DT_SYMTAB]);
  const char *strtab = (const void *) D_PTR (l, l_info[DT_STRTAB]);
  const ElfW(Sym) *sym = &symtab[ELFW(R_SYM) (reloc->r_info)];
  lookup_t result;

   /* Look up the target symbol.  If the normal lookup rules are not
      used don't look in the global scope.  */
  if (ELFW(ST_BIND) (sym->st_info) != STB_LOCAL
      && __builtin_expect (ELFW(ST_VISIBILITY) (sym->st_other), 0) == 0)
    {
      const struct r_found_version *version = NULL;

      if (l->l_info[VERSYMIDX (DT_VERSYM)] != NULL)
	{
	  const ElfW(Half) *vernum =
	    (const void *) D_PTR (l, l_info[VERSYMIDX (DT_VERSYM)]);
	  ElfW(Half) ndx = vernum[ELFW(R_SYM) (reloc->r_info)] & 0x7fff;
	  version = &l->l_versions[ndx];
	  if (version->hash == 0)
	    version = NULL;
	}

      result = _dl_lookup_symbol_x (strtab + sym->st_name, l, &sym,
				    l->l_scope, version, ELF_RTYPE_CLASS_PLT,
				    DL_LOOKUP_ADD_DEPENDENCY, NULL);
    }
  else
    {
      /* We already found the symbol.  The module (and therefore its load
	 address) is also known.  */
      result = l;
    }

  if (!sym)
    {
      atomic_store_relaxed (&td->arg, (void *) reloc->r_addend);
      /* This release store synchronizes with the ldar acquire load
	 instruction in _dl_tlsdesc_undefweak.  */
      atomic_store_release (&td->entry, _dl_tlsdesc_undefweak);
    }
  else
    {
#  ifndef SHARED
      CHECK_STATIC_TLS (l, result);
#  else
      if (!TRY_STATIC_TLS (l, result))
	{
	  void *p = _dl_make_tlsdesc_dynamic (result, sym->st_value
					      + reloc->r_addend);
	  atomic_store_relaxed (&td->arg, p);
	  /* This release store synchronizes with the ldar acquire load
	     instruction in _dl_tlsdesc_dynamic.  */
	  atomic_store_release (&td->entry, _dl_tlsdesc_dynamic);
	}
      else
#  endif
	{
	  void *p = (void*) (sym->st_value + result->l_tls_offset
			     + reloc->r_addend);
	  atomic_store_relaxed (&td->arg, p);
	  /* This release store synchronizes with the ldar acquire load
	     instruction in _dl_tlsdesc_return_lazy.  */
	  atomic_store_release (&td->entry, _dl_tlsdesc_return_lazy);
	}
    }

  _dl_tlsdesc_wake_up_held_fixups ();
}

/* This function is used to avoid busy waiting for other threads to
   complete the lazy relocation.  Once another thread wins the race to
   relocate a TLS descriptor, it sets the descriptor up such that this
   function is called to wait until the resolver releases the
   lock.  */

void
attribute_hidden
_dl_tlsdesc_resolve_hold_fixup (struct tlsdesc *td, void *caller)
{
  /* Maybe we're lucky and can return early.  */
  if (caller != atomic_load_relaxed (&td->entry))
    return;

  /* Locking here will stop execution until the running resolver runs
     _dl_tlsdesc_wake_up_held_fixups(), releasing the lock.

     FIXME: We'd be better off waiting on a condition variable, such
     that we didn't have to hold the lock throughout the relocation
     processing.  */
  __rtld_lock_lock_recursive (GL(dl_load_lock));
  __rtld_lock_unlock_recursive (GL(dl_load_lock));
}


/* Unmap the dynamic object, but also release its TLS descriptor table
   if there is one.  */

void
_dl_unmap (struct link_map *map)
{
  _dl_unmap_segments (map);

#ifdef SHARED
  if (map->l_mach.tlsdesc_table)
    htab_delete (map->l_mach.tlsdesc_table);
#endif
}
