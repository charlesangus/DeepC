CXX = g++
LINK = g++
STD = -std=c++11
FASTNOISEDIR = /media/sf_git/FastNoise/
CXX_FLAGS = -g -c \
            -DUSE_GLEW \
            -I$(NDK_STUB)$(1)/include \
            -I$(FASTNOISEDIR) \
            -DFN_EXAMPLE_PLUGIN -fPIC -msse
LINK_FLAGS = -L$(NDK_STUB)$(1) \
             -L$(FASTNOISEDIR) \
             -L./ \
             -shared
LIBS = -lDDImage \
       -lFastNoise

# plugins will be built against each of these nuke versions
NVS = 11.2v5 11.3v1

# we assume that each Nuke version lives in a folder called NukeXX.XvX
# in the same parent dir, so we can simply append to a stub
NDK_STUB = /usr/local/Nuke

SRC_DIR = src
OBJ_DIR_STUB = obj
PLUGIN_DIR_STUB = plugin
# $(1) will be subbed with the Nuke version
OBJ_DIR = $(OBJ_DIR_STUB)/Linux/$(1)
PLUGIN_DIR = $(PLUGIN_DIR_STUB)/Linux/$(1)

# GET ALL THE SOURCE FILES
SRC_FILES = $(wildcard $(SRC_DIR)/*.cpp)

# GET ALL THE PLUGIN FILES TO GENERATE
# FOR EACH SOURCE FILE, sub cpp with so
# FOR EACH NUKE VERSION
# SUB IN NUKE VERSION
PLUGIN_FILES = $(foreach NV, $(NVS), $(patsubst $(SRC_DIR)/%.cpp, $(PLUGIN_DIR_STUB)/Linux/$(NV)/%.so, $(SRC_FILES)))
$(info $$PLUGIN_FILES is [${PLUGIN_FILES}])
all: $(PLUGIN_FILES)

.SECONDARY:

# GENERIC OBJ - WILL BE FILLED IN FOR EACH NUKE VERSION
define OBJ_TEMPLATE
$(OBJ_DIR)/%.o: $$(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(STD) $(CXX_FLAGS) -o $$@ $$<
$(OBJ_DIR):
	mkdir -p $$@
endef

# GENERIC PLUGIN - WILL BE FILLED IN FOR EACH NUKE VERSION
define PLG_TEMPLATE
$(PLUGIN_DIR)/%.so: $(OBJ_DIR)/%.o | $(PLUGIN_DIR)
	$(LINK) $(STD) $(LINK_FLAGS) -o $$@ $$< $(LIBS)
$(PLUGIN_DIR):
	mkdir -p $$@
endef

# print the substituted templates
# $(foreach NV,$(NVS),$(info $(call OBJ_TEMPLATE,$(NV))))
# $(foreach NV,$(NVS),$(info $(call PLG_TEMPLATE,$(NV))))

# sub in the templates
$(foreach NV,$(NVS),$(eval $(call OBJ_TEMPLATE,$(NV))))
$(foreach NV,$(NVS),$(eval $(call PLG_TEMPLATE,$(NV))))

clean:
	rm -rf $(PLUGIN_FILES) $(OBJ_DIR_STUB)/*/*/*.o
