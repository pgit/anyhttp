#
# The dependencies of this project have been moved into another Dockerfile that can be built
# separately. This image can be pushed to the HUB, speeding up codespace creation considerably.
#
# FROM docker.io/psedoc/anyhttp:latest
FROM anyhttp:1.1
# FROM anyhttp:nghttp2-v1.64.0

#
# enable persistent bash history in container
#
# https://code.visualstudio.com/remote/advancedcontainers/persist-bash-history
#
# To actually make it persistent across re-creation of the container, you need to
# mount a volume to /commandhistory. You can do this in devcontainer.json like this:
#
# 	"mounts": [
# 		"source=cpp-devcontainer-bashhistory,target=/commandhistory,type=volume"
#   ]
#
ARG USERNAME=vscode
RUN SNIPPET="export PROMPT_COMMAND='history -a' && export HISTFILE=/commandhistory/.bash_history" \
    && mkdir /commandhistory \
    && touch /commandhistory/.bash_history \
    && chown -R $USERNAME /commandhistory \
    && echo "$SNIPPET" >> "/home/$USERNAME/.bashrc"

RUN echo "export LESS=RS" >/etc/profile.d/less.sh

SHELL ["/bin/bash", "-c"]
