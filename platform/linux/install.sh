#!/bin/sh
set -eu

die() {
    printf 'Folio: %s\n' "$*" >&2
    exit 1
}

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
data=${XDG_DATA_HOME:-"$HOME/.local/share"}
bin=${FOLIO_BIN_DIR:-"$HOME/.local/bin"}
case "$data" in /*) ;; *) die 'XDG_DATA_HOME must be an absolute path.' ;; esac
case "$bin" in /*) ;; *) die 'FOLIO_BIN_DIR must be an absolute path.' ;; esac
app="$data/folio"
binary="$app/folio"
launcher="$data/applications/org.folio.viewer.desktop"
icon="$data/icons/hicolor/128x128/apps/org.folio.viewer.png"
link="$bin/folio"

refresh_desktop() {
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$data/applications" >/dev/null 2>&1 || :
    fi

    case ":${XDG_CURRENT_DESKTOP:-}:" in
        *:KDE:*|*:kde:*)
            if command -v kbuildsycoca6 >/dev/null 2>&1; then
                kbuildsycoca6 >/dev/null 2>&1 || :
            elif command -v kbuildsycoca5 >/dev/null 2>&1; then
                kbuildsycoca5 >/dev/null 2>&1 || :
            fi
            ;;
    esac
}

owned_link() {
    [ -L "$link" ] && [ "$(readlink "$link")" = "$binary" ]
}

case "${1:-}" in
    --uninstall)
        if [ -f "$app/.installed-by-folio" ]; then
            if owned_link; then rm -- "$link"; fi
            for installed in "$binary" "$launcher" "$icon" "$app/.installed-by-folio"; do
                if [ -e "$installed" ] || [ -L "$installed" ]; then rm -- "$installed"; fi
            done
            rmdir -- "$app" 2>/dev/null || :
            refresh_desktop
            printf 'Folio was uninstalled.\n'
        else
            printf 'Folio is already uninstalled for this user.\n'
        fi
        exit 0
        ;;
    '') ;;
    *) die "Usage: sh install.sh [--uninstall]" ;;
esac

[ -f "$here/folio" ] && [ -f "$here/folio.png" ] ||
    die 'Extract the release archive before running the installer.'
[ "$(uname -s)" = Linux ] || die 'This installer requires Linux.'
case "$(uname -m):$(cat "$here/architecture")" in
    x86_64:x86_64|aarch64:aarch64) ;;
    *) die 'This archive is for a different CPU architecture.' ;;
esac
if [ -e "$app" ] && [ ! -f "$app/.installed-by-folio" ]; then
    die "$app already exists and belongs to another installation."
fi
if [ -e "$link" ] || [ -L "$link" ]; then
    owned_link || die "$link is already in use."
fi
if [ ! -f "$app/.installed-by-folio" ] && { [ -e "$launcher" ] || [ -e "$icon" ]; }; then
    die 'Another Folio installation already provides the desktop launcher or icon.'
fi

mkdir -p "$app" "$bin" "$(dirname -- "$launcher")" "$(dirname -- "$icon")"
temporary=$(mktemp -d "$app/.install.XXXXXX")
trap 'find "$temporary" -depth -delete' EXIT
trap 'exit 1' HUP INT TERM
install -m 755 "$here/folio" "$temporary/folio"
install -m 644 "$here/folio.png" "$temporary/folio.png"

# Exec values undergo both desktop-entry decoding and command-line unquoting.
exec_path=$(printf '%s' "$binary" | sed -e 's/\\/\\\\\\\\/g' -e 's/["$`]/\\\\&/g' -e 's/%/%%/g')
cat > "$temporary/folio.desktop" <<EOF_DESKTOP
[Desktop Entry]
Type=Application
Name=Folio
GenericName=PDF Viewer
Comment=Read PDF documents
Exec="$exec_path" %f
Icon=org.folio.viewer
Terminal=false
Categories=Office;Viewer;
MimeType=application/pdf;
EOF_DESKTOP

touch "$app/.installed-by-folio"
mv -f -- "$temporary/folio" "$binary"
mv -f -- "$temporary/folio.desktop" "$launcher"
mv -f -- "$temporary/folio.png" "$icon"
if ! owned_link; then ln -s -- "$binary" "$link"; fi
refresh_desktop
printf 'Installed Folio. Open it from the application menu or run %s\n' "$link"
