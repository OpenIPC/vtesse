/*
 * Self-install (#50, #51, #52): "vtesse --install" copies the running binary,
 * writes /etc/vtesse.conf from command-line keys, the init script for the
 * detected system and starts the service; "vtesse --uninstall" undoes it.
 * The service scripts are built into the binary (scripts/ at build time).
 */
#ifndef VTESSE_CLI_INSTALL_H
#define VTESSE_CLI_INSTALL_H

/* 1 when argv asks for --install or --uninstall */
int vi_wanted(int argc, char **argv);

/* runs --install / --uninstall; returns the exit code */
int vi_main(int argc, char **argv);

/* the built-in copy of scripts/<name>, or NULL */
const char *vi_script(const char *name);

/* path of the running binary (/proc/self/exe), "" if unknown */
const char *vi_self_path(void);

#endif
