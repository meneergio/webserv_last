NAME    = webserv

CXX     = c++
CXXFLAGS = -Wall -Wextra -Werror -std=c++98 -I include

# Platform detectie: kies de juiste Server implementatie
UNAME := $(shell uname)
ifeq ($(UNAME), Darwin)
    SERVER_SRC = src/Server.cpp
else
    SERVER_SRC = src/Server_linux.cpp
endif

SRCS    = src/main.cpp \
          src/ConfigParser.cpp \
          $(SERVER_SRC) \
          src/Request.cpp \
          src/Response.cpp \
          src/Cgi.cpp

OBJS    = $(SRCS:.cpp=.o)

all: $(NAME)

$(NAME): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(NAME) $(OBJS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS)

fclean: clean
	rm -f $(NAME)

re: fclean all

.PHONY: all clean fclean re
