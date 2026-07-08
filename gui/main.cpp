// MSXFileForge - native GUI (Dear ImGui + SDL2 + OpenGL3).
//
// Open an MSX disk image (.dsk / raw / .dmk), a cartridge ROM, or a .cas tape by
// dragging it onto the window or typing a path. Browse the FAT12 filesystem,
// inspect any file as hex, detokenize BASIC, read BLOAD headers, identify ROM
// mappers, and inspect the physical sector layout of protected / RAW images.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_opengl.h>
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"

#include "../core/disk_image.h"
#include "../core/fat12.h"
#include "../core/msxfile.h"
#include "../core/rom.h"
#include "../core/cas.h"

#include <string>
#include <vector>
#include <cstdio>
#include <cstring>

using namespace msx;

enum class Mode { None, Disk, Rom, Cas };

struct App {
    Mode mode = Mode::None;
    std::string loadedPath;
    std::string status = "Drop an MSX .dsk / .dmk / .rom / .cas here, or type a path above.";

    // Disk
    DiskImage disk;
    Fat12 fs;
    bool fsMounted = false;
    std::vector<DirEntry> listing;    // current directory listing
    std::string cwd = "/";
    bool showDeleted = false;

    // Selected file
    int selected = -1;
    std::string selName;
    std::vector<uint8_t> selData;
    MsxFileType selType = MsxFileType::Unknown;
    std::string detok;
    BloadHeader bload;

    // Rom / Cas
    std::vector<uint8_t> blob;
    RomInfo rom;
    std::vector<CasBlock> cas;

    char pathInput[1024] = {0};
};

static std::vector<uint8_t> slurp(const std::string& p) {
    std::vector<uint8_t> v;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0) { v.resize((size_t)n); if (fread(v.data(),1,(size_t)n,f)!=(size_t)n) v.clear(); }
    fclose(f);
    return v;
}

static bool hasExt(const std::string& p, const char* e) {
    size_t n = strlen(e);
    if (p.size() < n) return false;
    std::string t = p.substr(p.size()-n);
    for (auto& c : t) c = (char)tolower((unsigned char)c);
    return t == e;
}

static void selectFile(App& a, int idx) {
    a.selected = idx;
    a.detok.clear();
    a.bload = BloadHeader{};
    if (idx < 0 || idx >= (int)a.listing.size()) return;
    const DirEntry& e = a.listing[idx];
    a.selName = e.name;
    if (e.isDir()) { a.selData.clear(); a.selType = MsxFileType::Unknown; return; }
    a.selData = a.fs.readFile(e);
    a.selType = detectType(a.selData);
    if (a.selType == MsxFileType::BasicTokenized) a.detok = detokenizeBasic(a.selData);
    if (a.selType == MsxFileType::BloadBinary)    a.bload = parseBloadHeader(a.selData);
}

static void refreshListing(App& a) {
    a.listing = a.fs.list(a.cwd, a.showDeleted);
    a.selected = -1; a.selData.clear(); a.detok.clear();
    a.selType = MsxFileType::Unknown;
}

static void openPath(App& a, const std::string& path) {
    a.loadedPath = path;
    a.mode = Mode::None;
    a.fsMounted = false;
    a.selected = -1; a.selData.clear(); a.detok.clear();
    a.listing.clear(); a.cas.clear(); a.blob.clear();
    a.cwd = "/";

    if (hasExt(path, ".rom") || hasExt(path, ".mx1") || hasExt(path, ".mx2")) {
        a.blob = slurp(path);
        if (a.blob.empty()) { a.status = "Cannot read ROM: " + path; return; }
        a.rom = analyzeRom(a.blob);
        a.mode = Mode::Rom;
        a.status = "Loaded ROM (" + std::to_string(a.blob.size()) + " bytes)";
        return;
    }
    if (hasExt(path, ".cas")) {
        a.blob = slurp(path);
        if (a.blob.empty()) { a.status = "Cannot read CAS: " + path; return; }
        a.cas = parseCas(a.blob);
        a.mode = Mode::Cas;
        a.status = "Loaded CAS (" + std::to_string(a.cas.size()) + " blocks)";
        return;
    }
    // Default: treat as a disk image.
    std::string err;
    if (!a.disk.load(path, &err)) { a.status = "Load error: " + err; return; }
    a.mode = Mode::Disk;
    a.fsMounted = a.fs.mount(a.disk, &err);
    if (a.fsMounted) refreshListing(a);
    else a.listing.clear();
    a.status = std::string("Loaded ") + to_string(a.disk.format()) +
               (a.disk.isProtected() ? "  [PROTECTED]" : "") +
               (a.fsMounted ? "  FAT12 ok" : ("  no FS: " + err));
}

// ---- Hex view -------------------------------------------------------------
static void drawHex(const std::vector<uint8_t>& d, const char* id) {
    ImGui::BeginChild(id, ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clip;
    int rows = (int)((d.size() + 15) / 16);
    clip.Begin(rows);
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            size_t base = (size_t)r * 16;
            char line[128];
            int off = snprintf(line, sizeof line, "%06zX  ", base);
            ImGui::TextUnformatted(line, line + off);
            ImGui::SameLine(0, 0);
            std::string hex, asc;
            for (int c = 0; c < 16; ++c) {
                if (base + c < d.size()) {
                    char h[4]; snprintf(h, sizeof h, "%02X ", d[base + c]);
                    hex += h;
                    uint8_t ch = d[base + c];
                    asc += (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
                } else { hex += "   "; asc += ' '; }
                if (c == 7) hex += ' ';
            }
            ImGui::TextUnformatted(hex.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", asc.c_str());
        }
    }
    ImGui::EndChild();
}

// ---- Panels ---------------------------------------------------------------
static void drawDiskInfoBar(App& a) {
    ImGui::TextDisabled("Format:"); ImGui::SameLine();
    ImGui::TextUnformatted(to_string(a.disk.format()));
    ImGui::SameLine(); ImGui::TextDisabled(" | Geometry:"); ImGui::SameLine();
    ImGui::Text("%dc x %dh x %ds", a.disk.cylinders(), a.disk.heads(), a.disk.sectorsPerTrack());
    if (a.disk.isProtected()) {
        ImGui::SameLine(); ImGui::TextColored(ImVec4(1,0.5f,0.2f,1), " [PROTECTED]");
    }
    if (a.fsMounted) {
        ImGui::SameLine(); ImGui::TextDisabled(" | FAT12:"); ImGui::SameLine();
        ImGui::Text("OEM '%s'  free %ld KB", a.fs.oemName().c_str(), a.fs.freeBytes()/1024);
    }
}

static void drawFileTree(App& a) {
    ImGui::Checkbox("Show deleted", &a.showDeleted);
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) refreshListing(a);
    ImGui::Separator();
    if (!a.fsMounted) { ImGui::TextDisabled("No FAT12 filesystem."); return; }

    ImGui::Text("%s", a.cwd.c_str());
    if (a.cwd != "/") {
        if (ImGui::Selectable(".. (up)")) {
            size_t slash = a.cwd.find_last_of('/');
            a.cwd = (slash == 0 || slash == std::string::npos) ? "/" : a.cwd.substr(0, slash);
            refreshListing(a);
        }
    }
    for (int i = 0; i < (int)a.listing.size(); ++i) {
        const DirEntry& e = a.listing[i];
        if (e.isVolume()) continue;
        char label[96];
        snprintf(label, sizeof label, "%s %-13s %8u  [%s]",
                 e.isDir() ? "[DIR]" : "     ", e.name.c_str(), e.size, e.attrString().c_str());
        bool sel = (i == a.selected);
        if (ImGui::Selectable(label, sel)) {
            if (e.isDir() && e.name != "." && e.name != "..") {
                a.cwd = (a.cwd == "/" ? "/" : a.cwd + "/") + e.name;
                if (a.cwd.substr(0,2) == "//") a.cwd = a.cwd.substr(1);
                refreshListing(a);
            } else {
                selectFile(a, i);
            }
        }
        if (e.deleted) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "del"); }
    }
}

static void drawPreview(App& a) {
    if (ImGui::BeginTabBar("preview")) {
        // File-oriented tabs
        if (a.selected >= 0 && !a.selData.empty()) {
            ImGui::Text("%s  (%zu bytes)  -  %s", a.selName.c_str(),
                        a.selData.size(), typeName(a.selType));
            if (a.selType == MsxFileType::BasicTokenized && ImGui::BeginTabItem("BASIC")) {
                ImGui::BeginChild("basic", ImVec2(0,0), true);
                ImGui::TextUnformatted(a.detok.c_str());
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (a.selType == MsxFileType::BloadBinary && ImGui::BeginTabItem("BLOAD")) {
                ImGui::Text("start = 0x%04X", a.bload.start);
                ImGui::Text("end   = 0x%04X", a.bload.end);
                ImGui::Text("exec  = 0x%04X", a.bload.exec);
                ImGui::Text("size  = %u bytes", a.bload.end - a.bload.start + 1);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Hex")) { drawHex(a.selData, "filehex"); ImGui::EndTabItem(); }
        } else {
            ImGui::TextDisabled("Select a file to preview.");
        }
        // Whole-image hex
        if (ImGui::BeginTabItem("Raw image")) {
            ImGui::Text("Container bytes: %zu", a.disk.raw().size());
            drawHex(a.disk.raw(), "rawhex");
            ImGui::EndTabItem();
        }
        // Protection inspector
        if (ImGui::BeginTabItem("Protection / sectors")) {
            ImGui::Text("Physical sectors: %zu   Protected: %s",
                        a.disk.sectors().size(), a.disk.isProtected() ? "YES" : "no");
            if (a.disk.isProtected())
                ImGui::TextColored(ImVec4(1,0.6f,0.2f,1), "%s", a.disk.protectionReport().c_str());
            ImGui::Separator();
            ImGui::BeginChild("sec", ImVec2(0,0), true);
            if (ImGui::BeginTable("sectors", 6, ImGuiTableFlags_RowBg|ImGuiTableFlags_Borders|ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("C"); ImGui::TableSetupColumn("H");
                ImGui::TableSetupColumn("S"); ImGui::TableSetupColumn("Size");
                ImGui::TableSetupColumn("LBA"); ImGui::TableSetupColumn("Flags");
                ImGui::TableHeadersRow();
                for (auto& s : a.disk.sectors()) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%d", s.cyl);
                    ImGui::TableNextColumn(); ImGui::Text("%d", s.head);
                    ImGui::TableNextColumn(); ImGui::Text("%d", s.sec);
                    ImGui::TableNextColumn(); ImGui::Text("%zu", s.size);
                    ImGui::TableNextColumn(); ImGui::Text("%ld", s.lba);
                    ImGui::TableNextColumn();
                    std::string f;
                    if (s.nonStandard) f += "NONSTD ";
                    if (s.deleted) f += "DELETED ";
                    if (s.idCrcError) f += "IDCRC ";
                    if (s.dataCrcError) f += "DATACRC ";
                    if (f.empty()) ImGui::TextDisabled("-");
                    else ImGui::TextColored(ImVec4(1,0.6f,0.2f,1), "%s", f.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

static void drawRom(App& a) {
    ImGui::Text("ROM: %s", a.loadedPath.c_str());
    ImGui::Separator();
    ImGui::Text("Size    : %ld bytes  (%d x 16KB pages)", a.rom.size, a.rom.pages16k);
    ImGui::Text("Header  : 'AB' @0x0000 %s   @0x4000 %s",
                a.rom.hasHeaderAt0?"yes":"no", a.rom.hasHeaderAt4000?"yes":"no");
    if (a.rom.hasHeaderAt0 || a.rom.hasHeaderAt4000)
        ImGui::Text("Entries : INIT=%04X STATEMENT=%04X DEVICE=%04X TEXT=%04X",
                    a.rom.init, a.rom.statement, a.rom.device, a.rom.text);
    ImGui::Text("Mapper  : %s", mapperName(a.rom.mapper));
    ImGui::Separator();
    drawHex(a.blob, "romhex");
}

static void drawCas(App& a) {
    ImGui::Text("CAS: %s  (%zu blocks)", a.loadedPath.c_str(), a.cas.size());
    ImGui::Separator();
    if (ImGui::BeginTable("cas", 4, ImGuiTableFlags_RowBg|ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Offset"); ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Length");
        ImGui::TableHeadersRow();
        for (auto& b : a.cas) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("0x%06lX", b.offset);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(casKindName(b.kind));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(b.filename.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%ld", b.dataLength);
        }
        ImGui::EndTable();
    }
}

int main(int argc, char** argv) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL init: %s\n", SDL_GetError()); return 1; }
    const char* glsl = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_Window* win = SDL_CreateWindow("MSXFileForge",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1200, 760,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    SDL_GL_MakeCurrent(win, gl);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(win, gl);
    ImGui_ImplOpenGL3_Init(glsl);

    App app;
    if (argc > 1 && argv[1][0]) {
        strncpy(app.pathInput, argv[1], sizeof(app.pathInput) - 1);
        openPath(app, argv[1]);
    }

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                ev.window.windowID == SDL_GetWindowID(win)) running = false;
            if (ev.type == SDL_DROPFILE) {
                std::string p = ev.drop.file;
                SDL_free(ev.drop.file);
                openPath(app, p);
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##main", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Path bar
        ImGui::SetNextItemWidth(-120);
        ImGui::InputText("##path", app.pathInput, sizeof(app.pathInput));
        ImGui::SameLine();
        if (ImGui::Button("Open", ImVec2(110, 0)) && app.pathInput[0])
            openPath(app, app.pathInput);
        ImGui::TextDisabled("%s", app.status.c_str());
        ImGui::Separator();

        if (app.mode == Mode::Disk) {
            drawDiskInfoBar(app);
            ImGui::Separator();
            float leftW = 430.0f;
            ImGui::BeginChild("left", ImVec2(leftW, 0), true);
            drawFileTree(app);
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("right", ImVec2(0, 0), true);
            drawPreview(app);
            ImGui::EndChild();
        } else if (app.mode == Mode::Rom) {
            drawRom(app);
        } else if (app.mode == Mode::Cas) {
            drawCas(app);
        } else {
            ImGui::TextDisabled("Nothing loaded.");
            ImGui::Bullet(); ImGui::TextUnformatted("Disk: .dsk, raw, .dmk (protected/RAW-aware)");
            ImGui::Bullet(); ImGui::TextUnformatted("Cartridge: .rom (.mx1/.mx2)");
            ImGui::Bullet(); ImGui::TextUnformatted("Tape: .cas");
        }

        ImGui::End();

        ImGui::Render();
        ImGuiIO& io = ImGui::GetIO();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.09f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(win);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
