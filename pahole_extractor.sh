printf "#ifndef SHADOW_STRUCTS_H\n#define SHADOW_STRUCTS_H\n\nstruct mutex;\nstruct eventpoll;\n" > shadow_structs.h
pahole -C eventpoll /sys/kernel/btf/vmlinux >> shadow_structs.h
sed -i 's/struct eventpoll /struct eventpoll_shadow /g' shadow_structs.h
pahole -C epoll_filefd /sys/kernel/btf/vmlinux >> shadow_structs.h
pahole -C epitem /sys/kernel/btf/vmlinux >> shadow_structs.h
sed -i 's/struct epoll_filefd /struct epoll_filefd_shadow /g' shadow_structs.h
sed -i 's/struct epitem /struct epitem_shadow /g' shadow_structs.h
printf "\n#endif // SHADOW_STRUCTS_H\n" >> shadow_structs.h
cat shadow_structs.h
