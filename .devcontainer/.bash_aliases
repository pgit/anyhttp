alias gga="git log --graph --oneline --decorate --all"
alias gs="git status"
alias gd="git diff"
alias gds="git diff --staged"
alias tma="tmux attach"

mkdir -p ~/.config/{btop,htop}
[[ -f ~/.config/btop/btop.conf ]] || ln -sf /commandhistory/btop.conf ~/.config/btop/
[[ -f ~/.config/htop/htoprc    ]] || ln -sf /commandhistory/htoprc    ~/.config/htop/

[[ -f /commandhistory/.bashrc ]] && source /commandhistory/.bashrc
