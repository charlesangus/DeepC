CXX ?= g++
LINK ?= g++
NDKDIR ?= /usr/local/Nuke11.1v6/
CXXFLAGS ?= -g -c \
            -DUSE_GLEW \
            -I$(NDKDIR)/include \
            -DFN_EXAMPLE_PLUGIN -fPIC -msse 
LINKFLAGS ?= -L$(NDKDIR) \
             -L./ 
LIBS ?= -lDDImage
LINKFLAGS += -shared

SRC_DIR = src
OBJ_DIR = obj
PLUGIN_DIR = plugin
SRC_FILES = $(wildcard $(SRC_DIR)/*.cpp)
OBJ_FILES = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.os, $(SRC_FILES))
PLUGIN_FILES = $(patsubst $(SRC_DIR)/%.cpp, $(PLUGIN_DIR)/%.so, $(SRC_FILES))

all: ndkexists what $(PLUGIN_FILES)

ndkexists:
	@if test -d $(NDKDIR); \
	then echo "using NDKDIR from ${NDKDIR}"; \
	else echo "NDKDIR dir not found! Please set NDKDIR"; exit 2; \
	fi

what:
	@echo $(SRC_FILES); \
	echo $(OBJ_FILES); \
	echo $(PLUGIN_FILES); \
	echo $(PLUGIN_DIR)/%.so;

# make object files from source
$(OBJ_DIR)/%.os: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -o $(@) $<

# link object files into plugins
$(PLUGIN_DIR)/%.so: $(OBJ_DIR)/%.os
	$(LINK) $(LINKFLAGS) $(LIBS) -o $(@) $<


clean:
	rm -rf $(OBJ_DIR)/*.os \
	       $(PLUGIN_DIR)/*.so

