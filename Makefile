all:
	gcc -g raw_ethernet_send_bw.c -o raw_ethernet_bw -include config.h get_clock.c multicast_resources.c perftest_communication.c perftest_parameters.c perftest_resources.c raw_ethernet_resources.c -libverbs -lpthread -lm -lrdmacm -libumad
clean:
	rm *.o raw_ethernet_bw
