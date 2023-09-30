https: main_https.c
	gcc main.c -o main_https mongoose/mongoose.c -I mongoose/ ../git/libgittest/libgit2/build/libgit2.a -I ../git/libgittest/libgit2/include -DMG_ENABLE_IPV6=1 -DMG_ENABLE_OPENSSL=1 -lssl -lcrypto -lm -lz -g


main: main.c
	gcc main.c -o main mongoose/mongoose.c -I mongoose/ -DMG_ENABLE_IPV6=1

run: main
	./main

run_https: https
	authbind --deep ./main_https
