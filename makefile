#main : main.c
#	gcc $^ -o $@ -lm

STD = -std=gnu11

main.out : main.c
	$(CC) $^ -o $@ -pthread -I/usr/include/libxml2 -lm -lxml2 -lcurl -lyaml -ggdb `pkg-config --cflags --libs libnotify` $(STD)

