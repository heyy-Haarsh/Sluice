all: level1 level2

level1:
	g++ -std=c++17 -Wall -o level1_basic/server level1_basic/server.cpp
	g++ -std=c++17 -Wall -o level1_basic/client level1_basic/client.cpp

level2:
	g++ -std=c++17 -Wall -o level2_epoll/server level2_epoll/server.cpp

level2:
	g++ -std=c++17 -Wall -o level3_gateway/server level3_gateway/server.cpp
	g++ -std=c++17 -Wall -o level3_gateway/gateway level3_gateway/gateway.cpp

clean:
	rm -f level1_basic/server level1_basic/client level2_epoll/server
