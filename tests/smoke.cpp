#define USE_FB0
#define main flipper_term_main
#include "../flipper-term.cpp"
#undef main
#include <cassert>
#include <iostream>
int main() {
    FrameBuffer f; Canvas c(f); c.clear();
    c.set_pixel(3,4); assert(f.get(3,4)); c.invert_rect(3,4,1,1); assert(!f.get(3,4));
    c.draw_line(0,0,5,5); assert(f.get(5,5)); c.fill_rect(125,62,8,8); assert(f.get(127,63));
    c.clear(); c.draw_string(0,0,"Flipper",kFontSmall); bool has_pixel=false; for(auto v:f.px) has_pixel |= v!=0; assert(has_pixel);
    auto root=fs::temp_directory_path()/ ("flipper-term-test-"+std::to_string((long long)getpid()));
    g_settings.vm_dir=root.string(); g_settings.ensure_dirs(); g_settings.zoom=5; g_settings.save();
    Settings read; read.vm_dir=root.string(); read.load(); assert(read.zoom==5);
    DeviceApp app("125 kHz RFID"); app.handle_input(InputEvent::Ok,1); assert(app.page=="reading");
    app.handle_input(InputEvent::Ok,2); assert(app.page=="result"); app.handle_input(InputEvent::Ok,3);
    assert(app.page=="message"); assert(list_files((root/"lfrfid").string(),".rfid").size()==1);
    DeviceApp bad("Bad USB"); write_text_file((root/"scripts"/"hello.txt").string(),"STRING hello\n");
    bad.handle_input(InputEvent::Ok,1); assert(bad.page=="files" && !bad.files.empty());
    bad.handle_input(InputEvent::Ok,2); assert(bad.page=="preview"); bad.handle_input(InputEvent::Ok,3); assert(bad.transmitting==0);
    GamesApp games; games.handle_input(InputEvent::Ok,0); assert(games.mode=="snake"); games.draw(c,181); assert(games.snake[0].first==4);
    SettingsApp settings; settings.sel=3; settings.handle_input(InputEvent::Ok,1); assert(settings.confirm);
    settings.handle_input(InputEvent::Back,2); assert(fs::exists(root/"lfrfid"));
    settings.handle_input(InputEvent::Ok,3); settings.handle_input(InputEvent::Ok,4);
    assert(list_files((root/"lfrfid").string(),".rfid").empty());
    std::error_code ec; fs::remove_all(root,ec); std::cout << "smoke tests passed\n";
}
