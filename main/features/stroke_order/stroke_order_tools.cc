#include "stroke_order_tools.h"

#include <string>

#include "application.h"
#include "display/lcd_display.h"
#include "mcp_server.h"
#include "stroke_tool.h"

void RegisterStrokeOrderTools(LcdDisplay* lcd) {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.stroke.show",
        "在屏幕上演示一个汉字的笔顺：逐笔动画 + 拼音，支持约 9500 个常用汉字（含小学 1-6 "
        "年级字表）。传入要查询的汉字或包含它的句子，取其中最后一个汉字。",
        PropertyList({Property("char", kPropertyTypeString)}),
        [lcd](const PropertyList& properties) -> ToolResult {
            auto& store = StrokeStore::GetInstance();
            if (!store.Ready()) {
                return std::string("字库还没准备好，请确认 TF 卡已插入。");
            }
            const std::string input = properties["char"].value<std::string>();
            const int cp = StrokeStore::Utf8SingleCjk(input);
            const int idx = cp >= 0 ? store.FindByChar(static_cast<uint16_t>(cp)) : -1;
            if (idx < 0) {
                return std::string("这个字暂时没有笔顺数据，换一个常用汉字试试。");
            }
            std::string ch;
            ch += static_cast<char>(0xE0 | (cp >> 12));
            ch += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            ch += static_cast<char>(0x80 | (cp & 0x3F));
            Application::GetInstance().Schedule([lcd, idx]() { lcd->ShowStrokeScreen(idx); });
            return "正在屏幕上演示「" + ch + "」的笔顺：拼音 " + store.PinyinOf(idx) + "，共 " +
                   std::to_string(store.StrokeCountAt(idx)) + " 笔——" +
                   store.StrokeNamesText(idx) + "。动画播完会自动跳到下一个字。";
        });

    mcp.AddTool(
        "self.stroke.next",
        "在笔顺演示页翻字：dir 为 -1 切到上一个字，1 切到下一个字，同年级内循环。",
        PropertyList({Property("dir", kPropertyTypeInteger, -1, 1)}),
        [lcd](const PropertyList& properties) -> ToolResult {
            auto& store = StrokeStore::GetInstance();
            if (!store.Ready()) {
                return std::string("字库还没准备好，请确认 TF 卡已插入。");
            }
            const int dir = properties["dir"].value<int>() < 0 ? -1 : 1;
            Application::GetInstance().Schedule([lcd, dir]() { lcd->StrokeChangeChar(dir); });
            return dir > 0 ? std::string("已切换到下一个字。")
                           : std::string("已切换到上一个字。");
        });

    mcp.AddTool("self.stroke.close",
                "关闭笔顺演示页，回到正常聊天界面。用户说关掉或退出笔顺时调用。", PropertyList(),
                [lcd](const PropertyList&) -> ToolResult {
                    Application::GetInstance().Schedule([lcd]() { lcd->ShowStrokeScreen(false); });
                    return std::string("已关闭笔顺演示页。");
                });
}
