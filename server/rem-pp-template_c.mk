###############################################################################
#
# PP TEMPLATE (for a C PP)
#
###############################################################################

# release version of the PP relative to the main release (see var
# $(RELEASE_MAIN) in 'Makefile')
RELEASE_PP := 0

# Uncomment if the PP makes use of the functions offered by 'rem-tags.h'
# (reading music meta data - e.g. id3 - from music files)
#USE_REM_TAG := yes

# Uncomment if the PP makes use of the functions offered by 'rem-dbus.h' (to
# communicate with a music player via DBUS)
USE_REM_DBUS := yes

# PP specific compile and link flags
PP_CFLAGS := 
PP_LFLAGS := 

server: prereqs rem-pp-$(PP_NAME).c
	$(CC) -o remuco-$(PP_NAME) rem-pp-$(PP_NAME).c $(CFLAGS) $(LFLAGS) \
		$(PP_CFLAGS) $(PP_LFLAGS)
	$(STRIP) remuco-$(PP_NAME)
	chmod +x remuco-$(PP_NAME)

install: server
	$(INSTALL_PROG) remuco-$(PP_NAME) $(DESTDIR)$(BIN_DIR)
	
# When calling target 'dist' the file remuco-$(PP_NAME) will be inclueded in
# the distribution packages per default. If further files are needed they must
# be specified here.
PP_EXTRA_BIN_FILES :=
PP_EXTRA_LIB_FILES :=
