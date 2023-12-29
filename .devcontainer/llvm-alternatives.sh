#!/bin/bash
#
# Use 'update-alternatives' to enable installd LLVM packages as default.
#
[[ $1 ]] || exit 1
VERSION=$1
PRIORITY=100

CMD=(update-alternatives --install /usr/lib/llvm llvm /usr/lib/llvm-$VERSION $PRIORITY)

#
# read all symlinks in /usr/bin that link to /usr/lib/llvm-*
#
while read FILE
do
    DEST=$(readlink -f "$FILE")
    [[ $DEST == /usr/lib/llvm-$VERSION/* ]] || continue
    [[ $FILE == *-$VERSION ]] || continue
    TARGET=${FILE%*-$VERSION}
    BASENAME=${TARGET##*/}
    CMD+=(--slave "${FILE%*-$VERSION}" "$BASENAME" "$FILE")
done < <(find /usr/bin -maxdepth 1 -type l)

"${CMD[@]}"
