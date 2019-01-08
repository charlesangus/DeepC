CXX = g++
LINK = g++
STD = -std=c++11
FASTNOISEDIR = FastNoise
CXXFLAGS = -c \
           -I$(NDK_STUB)$(1)/include \
           -I$(FASTNOISEDIR) \
		   -Isrc \
           -fPIC \
           -msse -msse2 -msse3 -mssse3 -msse4 -msse4.1 -msse4.2 -mavx
LINK_FLAGS = -shared \
             -L$(NDK_STUB)$(1)
LIBS = -lDDImage

DEBUG ?= 0
ifeq (DEBUG, 1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O3
endif

# plugins will be built against each of these nuke versions
NVS = 11.2v5 11.3v1

# we assume that each Nuke version lives in a folder called NukeXX.XvX
# in the same parent dir, so we can simply append to a stub
NDK_STUB = /usr/local/Nuke

SRC_DIR = src
OBJ_DIR_STUB = obj
PLUGIN_DIR_STUB = plugin
RELEASE_DIR_STUB = release
# $(1) will be subbed with the Nuke version
OBJ_DIR = $(OBJ_DIR_STUB)/Linux/$(1)
PLUGIN_DIR = $(PLUGIN_DIR_STUB)/Linux/$(1)
RELEASE_DIR = $(RELEASE_DIR_STUB)/Linux/$(1)
ICON_DIR = icons
RELEASE_VERSIONS = $(foreach NV,$(NVS),release-$(NV))
# GET ALL THE SOURCE FILES
SRC_FILES = $(wildcard $(SRC_DIR)/*.cpp)

# GET ALL THE PLUGIN FILES TO GENERATE
# FOR EACH SOURCE FILE, sub cpp with so
# FOR EACH NUKE VERSION
# SUB IN NUKE VERSION
PLUGIN_FILES = $(foreach NV, $(NVS), $(patsubst $(SRC_DIR)/%.cpp, $(PLUGIN_DIR_STUB)/Linux/$(NV)/%.so, $(SRC_FILES)))
$(info $$PLUGIN_FILES is [${PLUGIN_FILES}])

.PHONY: all release clean clean-release

all: $(PLUGIN_FILES)

.SECONDARY:

# GENERIC OBJ - WILL BE FILLED IN FOR EACH NUKE VERSION
define OBJ_TEMPLATE
$(OBJ_DIR)/%.o: $$(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(STD) $(CXXFLAGS) -o $$@ $$<
$(OBJ_DIR):
	mkdir -p $$@
endef

# GENERIC PLUGIN - WILL BE FILLED IN FOR EACH NUKE VERSION
define PLG_TEMPLATE
$(PLUGIN_DIR)/%.so: $(OBJ_DIR)/%.o | $(PLUGIN_DIR)
	$(LINK) $(STD) $(LINK_FLAGS) -o $$@ $$< $(LIBS)
$(PLUGIN_DIR)/DeepCMWrapper.so: $(OBJ_DIR)/DeepCMWrapper.o $(OBJ_DIR)/DeepCWrapper.o | $(PLUGIN_DIR)
	$(LINK) $(STD) $(LINK_FLAGS) -o $$@ $$^ $(LIBS)
$(PLUGIN_DIR)/DeepCPMatte.so: $(OBJ_DIR)/DeepCPMatte.o $(OBJ_DIR)/DeepCMWrapper.o $(OBJ_DIR)/DeepCWrapper.o | $(PLUGIN_DIR)
	$(LINK) $(STD) $(LINK_FLAGS) -o $$@ $$^ $(LIBS)
$(PLUGIN_DIR)/DeepCID.so: $(OBJ_DIR)/DeepCID.o $(OBJ_DIR)/DeepCMWrapper.o $(OBJ_DIR)/DeepCWrapper.o | $(PLUGIN_DIR)
	$(LINK) $(STD) $(LINK_FLAGS) -o $$@ $$^ $(LIBS)
$(PLUGIN_DIR)/DeepCBlink.so: $(OBJ_DIR)/DeepCBlink.o | $(PLUGIN_DIR)
	$(LINK) $(STD) $(LINK_FLAGS) -o $$@ $$^ $(LIBS) -lRIPFramework
$(PLUGIN_DIR)/DeepCGrade.so: $(OBJ_DIR)/DeepCGrade.o $(OBJ_DIR)/DeepCWrapper.o | $(PLUGIN_DIR)
	$(LINK) $(STD) $(LINK_FLAGS) -o $$@ $$^ $(LIBS)
$(PLUGIN_DIR)/DeepCSaturation.so: $(OBJ_DIR)/DeepCSaturation.o $(OBJ_DIR)/DeepCWrapper.o | $(PLUGIN_DIR)
	$(LINK) $(STD) $(LINK_FLAGS) -o $$@ $$^ $(LIBS)
$(PLUGIN_DIR)/DeepCPNoise.so: $(OBJ_DIR)/DeepCPNoise.o $(OBJ_DIR)/DeepCMWrapper.o $(OBJ_DIR)/DeepCWrapper.o | $(PLUGIN_DIR)
	$(LINK) $(STD) $(LINK_FLAGS) -L$(FASTNOISEDIR)/build -o $$@ $$^ $(LIBS) -lFastNoise
$(PLUGIN_DIR):
	mkdir -p $$@
endef

release: $(RELEASE_VERSIONS)

define RELEASE_TEMPLATE
release-$(NV): $(PLUGIN_FILES)
	mkdir -p $(RELEASE_DIR)
	cp $(PLUGIN_DIR)/* $(RELEASE_DIR)/
	cp python/* $(RELEASE_DIR)
	mkdir -p $(RELEASE_DIR)/icons
	cp icons/DeepC*.png $(RELEASE_DIR)/icons/
	rm -rf $(RELEASE_DIR)/*Wrapper*.so
	cd $(RELEASE_DIR); \
	zip -r DeepC-Linux-$(1).zip *
	cp $(RELEASE_DIR)/*.zip $(RELEASE_DIR_STUB)/
endef


# print the substituted templates
# $(foreach NV,$(NVS),$(info $(call OBJ_TEMPLATE,$(NV))))
# $(foreach NV,$(NVS),$(info $(call PLG_TEMPLATE,$(NV))))

# sub in the templates
$(foreach NV,$(NVS),$(eval $(call OBJ_TEMPLATE,$(NV))))
$(foreach NV,$(NVS),$(eval $(call PLG_TEMPLATE,$(NV))))
$(foreach NV,$(NVS),$(eval $(call RELEASE_TEMPLATE,$(NV))))

clean:
	rm -rf $(PLUGIN_FILES) $(OBJ_DIR_STUB)/*/*/*.o
	rm -rf $(PLUGIN_FILES) $(PLUGIN_DIR_STUB)/*/*/*.so

clean-release:
	rm -rf $(RELEASE_DIR_STUB)/*

test:
	rm -rf /home/ndker/.nuke/DeepC-Linux-11.2v5
	rm -rf /home/ndker/.nuke/DeepC-Linux-11.2v5.zip
	cp $(RELEASE_DIR_STUB)/DeepC-Linux-11.2v5.zip /home/ndker/.nuke/DeepC-Linux-11.2v5.zip
	unzip -d /home/ndker/.nuke/DeepC-Linux-11.2v5 /home/ndker/.nuke/DeepC-Linux-11.2v5.zip
