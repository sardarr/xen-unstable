--- ../../linux-2.6.7/include/asm-ia64/page.h	2004-06-15 23:18:58.000000000 -0600
+++ include/asm-ia64/page.h	2005-04-01 12:56:37.000000000 -0700
@@ -12,6 +12,9 @@
 #include <asm/intrinsics.h>
 #include <asm/types.h>
 
+#ifndef __ASSEMBLY__
+#include <asm/flushtlb.h>
+#endif
 /*
  * PAGE_SHIFT determines the actual kernel page size.
  */
@@ -84,14 +87,22 @@
 #endif
 
 #ifndef CONFIG_DISCONTIGMEM
+#ifdef XEN
+#define pfn_valid(pfn)		(0)
+#else
 #define pfn_valid(pfn)		(((pfn) < max_mapnr) && ia64_pfn_valid(pfn))
-#define page_to_pfn(page)	((unsigned long) (page - mem_map))
-#define pfn_to_page(pfn)	(mem_map + (pfn))
+#endif
 #endif /* CONFIG_DISCONTIGMEM */
 
-#define page_to_phys(page)	(page_to_pfn(page) << PAGE_SHIFT)
+#define page_to_pfn(_page)  ((unsigned long)((_page) - frame_table))
+#define page_to_virt(_page) phys_to_virt(page_to_phys(_page))
+
+#define page_to_phys(_page)	(page_to_pfn(_page) << PAGE_SHIFT)
 #define virt_to_page(kaddr)	pfn_to_page(__pa(kaddr) >> PAGE_SHIFT)
 
+#define pfn_to_page(_pfn)   	(frame_table + (_pfn))
+#define phys_to_page(kaddr) 	pfn_to_page(((kaddr) >> PAGE_SHIFT))
+
 typedef union ia64_va {
 	struct {
 		unsigned long off : 61;		/* intra-region offset */
@@ -107,8 +118,25 @@
  * expressed in this way to ensure they result in a single "dep"
  * instruction.
  */
+#ifdef XEN
+typedef union xen_va {
+	struct {
+		unsigned long off : 60;
+		unsigned long reg : 4;
+	} f;
+	unsigned long l;
+	void *p;
+} xen_va;
+
+// xen/drivers/console.c uses __va in a declaration (should be fixed!)
+#define __pa(x)		({xen_va _v; _v.l = (long) (x); _v.f.reg = 0; _v.l;})
+#define __va(x)		({xen_va _v; _v.l = (long) (x); _v.f.reg = -1; _v.p;})
+//# define __pa(x)	((unsigned long)(((unsigned long)x) - PAGE_OFFSET))
+//# define __va(x)	((void *)((char *)(x) + PAGE_OFFSET))
+#else
 #define __pa(x)		({ia64_va _v; _v.l = (long) (x); _v.f.reg = 0; _v.l;})
 #define __va(x)		({ia64_va _v; _v.l = (long) (x); _v.f.reg = -1; _v.p;})
+#endif
 
 #define REGION_NUMBER(x)	({ia64_va _v; _v.l = (long) (x); _v.f.reg;})
 #define REGION_OFFSET(x)	({ia64_va _v; _v.l = (long) (x); _v.f.off;})
@@ -180,11 +208,19 @@
 # define __pgprot(x)	(x)
 #endif /* !STRICT_MM_TYPECHECKS */
 
+#ifdef XEN
+#define PAGE_OFFSET			0xf000000000000000
+#else
 #define PAGE_OFFSET			0xe000000000000000
+#endif
 
 #define VM_DATA_DEFAULT_FLAGS		(VM_READ | VM_WRITE |					\
 					 VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC |		\
 					 (((current->thread.flags & IA64_THREAD_XSTACK) != 0)	\
 					  ? VM_EXEC : 0))
 
+#ifdef XEN
+#define __flush_tlb()	do {} while(0);
+#endif
+
 #endif /* _ASM_IA64_PAGE_H */
