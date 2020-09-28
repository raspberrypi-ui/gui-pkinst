bin_PROGRAMS = gui-pkinst

gui_pkinst_CFLAGS = \
	-I$(top_srcdir) \
	-DPACKAGE_LIB_DIR=\""$(libdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)/gui-pkinst"\" \
	-DPACKAGE_UI_DIR=\""$(datadir)/gui-pkinst/ui"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	$(PACKAGE_CFLAGS) \
	$(G_CAST_CHECKS)

gui_pkinst_SOURCES = gui-pkinst.c

gui_pkinst_includedir = $(includedir)/gui-pkinst

gui_pkinst_include_HEADERS =

gui_pkinst_LDFLAGS = $(DYNAMIC_FLAGS)
gui_pkinst_DEPENDENCIES_EXTRA = $(BUILTIN_PLUGINS)

gui_pkinst_LDADD = \
		$(BUILTIN_PLUGINS) \
		$(PACKAGE_LIBS) \
		$(X11_LIBS) \
		$(INTLLIBS)

EXTRA_DIST =
