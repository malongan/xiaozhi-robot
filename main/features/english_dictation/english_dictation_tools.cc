#include "english_dictation_tools.h"

#include <string>

#include "application.h"
#include "display/lcd_display.h"
#include "english_player.h"
#include "english_tool.h"
#include "mcp_server.h"

void RegisterEnglishDictationTools(LcdDisplay* lcd) {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.english.dictation",
        "开始英语报听写：设备依次朗读单词，每个词之间间隔若干秒，学生在本子上写下听到的单词。"
        "unit 为课本单元号（1-7 四年级上，12-17 五年级上，18-23 五年级下，24-29 六年级上，"
        "30-33 六年级下，0 表示不限），topic 为词义分类（0 不限，1 水果，2 蔬菜，3 食物饮料，"
        "4 身体部位，5 颜色，6 数字，7 时间日期，8 动物，9 学校学习，10 场所地点，"
        "11 家居物品，12 人物职业，13 家人朋友，14 情绪感受，15 动作，16 形容词，"
        "17 自然天气，18 交通出行，19 运动爱好，20 节日文化，21 科技通信，22 购物金钱，"
        "23 健康安全，24 服装，25 国家语言，27 形状），unit 与 topic 可同时使用（取交集）。"
        "count 为词数（0 表示全部），interval 为间隔秒数（默认 10，0 表示手动：只播一遍当前词，"
        "等用户说下一个或点按钮才翻页），show_word 为 true 时屏幕上会显示单词拼写"
        "（默认只显示中文意思），speak 为播报语言：0 报英文，1 报中文，2 中英都报。"
        "运行中只想改语言、间隔或显示方式时不要重新开始，请优先调用 "
        "self.english.speak / self.english.interval。",
        PropertyList({Property("unit", kPropertyTypeInteger, 0, 34),
                      Property("topic", kPropertyTypeInteger, 0, 27),
                      Property("count", kPropertyTypeInteger, 0, 100),
                      Property("interval", kPropertyTypeInteger, 0, 120),
                      Property("show_word", kPropertyTypeBoolean, false),
                      Property("speak", kPropertyTypeInteger, 0, 2)}),
        [lcd](const PropertyList& properties) -> ToolResult {
            auto& store = EnglishStore::GetInstance();
            if (!store.Ready() || store.WordCount() == 0) {
                return std::string("英语词库还没准备好，请确认 TF 卡已插入并包含 english 目录。");
            }
            const int unit = properties["unit"].value<int>();
            const int topic = properties["topic"].value<int>();
            const int count = properties["count"].value<int>();
            const int interval = properties["interval"].value<int>();
            const bool show_word = properties["show_word"].value<bool>();
            const int speak = properties["speak"].value<int>();
            Application::GetInstance().Schedule([lcd, unit, topic, count, interval, show_word,
                                                 speak]() {
                lcd->ShowEnglishScreen(true);
                auto& player = EnglishPlayer::GetInstance();
                const auto want = static_cast<EnglishPlayer::Speak>(speak < 0 ? 0 : (speak % 3));
                // 同一轮听写里改语言/间隔/显示方式时只更新参数，不回到第一个词。
                const bool same_lesson = player.Active() && player.CurrentUnit() == unit &&
                                         player.CurrentTopic() == topic &&
                                         player.CurrentCount() == count &&
                                         player.CurrentMode() == EnglishPlayer::kDictation;
                if (same_lesson) {
                    player.SetSpeak(want);
                    player.SetInterval(interval);
                    player.SetShowWord(show_word);
                } else {
                    player.SetSpeak(want);
                    player.Start(unit, topic, count, interval, show_word);
                }
                lcd->UpdateEnglishScreen();
            });
            const int total = store.CountFilter(unit, topic);
            const int words = count > 0 && count < total ? count : total;
            if (interval <= 0) {
                return "开始报听写：共 " + std::to_string(words) +
                       " 个词，手动模式，每报完一个要等用户说“下一个”或点屏幕才翻页。";
            }
            return "开始报听写：共 " + std::to_string(words) + " 个词，每 " +
                   std::to_string(interval) +
                   " 秒一个，屏幕上会显示进度。设备会自己按间隔推进，接下来不要再调用翻页工具"
                   "。";
        });

    mcp.AddTool(
        "self.english.cards",
        "打开英语单词卡：屏幕上显示单词、音标和中文释义并朗读发音，"
        "每隔几秒自动翻到下一张，适合预习单词。"
        "unit 为课本单元号（1-7 四年级上，12-17 五年级上，18-23 五年级下，24-29 六年级上，"
        "30-33 六年级下，0 表示不限），topic 为词义分类（0 不限，1 水果，2 蔬菜，"
        "3 食物饮料，4 身体部位，5 颜色，6 数字，7 时间日期，8 动物，9 学校学习，"
        "10 场所地点，11 家居物品，12 人物职业，13 家人朋友，14 情绪感受，15 动作，"
        "16 形容词，17 自然天气，18 交通出行，19 运动爱好，20 节日文化，21 科技通信，"
        "22 购物金钱，23 健康安全，24 服装，25 国家语言，27 形状），count 为词数（0 "
        "表示全部），"
        "interval 为每张卡停留的秒数（默认 10，0 表示手动翻页）。",
        PropertyList({Property("unit", kPropertyTypeInteger, 0, 34),
                      Property("topic", kPropertyTypeInteger, 0, 27),
                      Property("count", kPropertyTypeInteger, 0, 100),
                      Property("interval", kPropertyTypeInteger, 0, 120)}),
        [lcd](const PropertyList& properties) -> ToolResult {
            auto& store = EnglishStore::GetInstance();
            if (!store.Ready() || store.WordCount() == 0) {
                return std::string("英语词库还没准备好，请确认 TF 卡已插入并包含 english 目录。");
            }
            const int unit = properties["unit"].value<int>();
            const int topic = properties["topic"].value<int>();
            const int count = properties["count"].value<int>();
            const int interval = properties["interval"].value<int>();
            Application::GetInstance().Schedule([lcd, unit, topic, count, interval]() {
                lcd->ShowEnglishScreen(true);
                auto& player = EnglishPlayer::GetInstance();
                const bool already =
                    player.Active() && player.CurrentUnit() == unit &&
                    player.CurrentTopic() == topic && player.CurrentCount() == count &&
                    player.Interval() == interval && player.CurrentMode() == EnglishPlayer::kCards;
                if (!already) {
                    player.Start(unit, topic, count, interval, true, EnglishPlayer::kCards);
                }
                lcd->UpdateEnglishScreen();
            });
            const int total = store.CountFilter(unit, topic);
            const int words = count > 0 && count < total ? count : total;
            if (interval <= 0) {
                return "开始单词卡：" + std::to_string(words) + " 个词，手动翻页。";
            }
            return "开始单词卡：" + std::to_string(words) + " 个词，每 " +
                   std::to_string(interval) + " 秒翻一张。";
        });

    mcp.AddTool("self.english.next",
                "听写页翻到下一个词。仅在用户明确要求“下一个/继续/跳过一个”时调用；"
                "听写进行中设备会自己按间隔推进，不要反复调用这个工具。",
                PropertyList(), [lcd](const PropertyList&) -> ToolResult {
                    Application::GetInstance().Schedule([lcd]() {
                        EnglishPlayer::GetInstance().RequestNext();
                        lcd->UpdateEnglishScreen();
                    });
                    return std::string("已到下一个词。");
                });

    mcp.AddTool("self.english.prev", "听写页回到上一个词。仅在用户明确要求“上一个/返回”时调用。",
                PropertyList(), [lcd](const PropertyList&) -> ToolResult {
                    Application::GetInstance().Schedule([lcd]() {
                        EnglishPlayer::GetInstance().RequestPrev();
                        lcd->UpdateEnglishScreen();
                    });
                    return std::string("已回到上一个词。");
                });

    mcp.AddTool("self.english.repeat", "重播当前单词的发音。", PropertyList(),
                [lcd](const PropertyList&) -> ToolResult {
                    Application::GetInstance().Schedule([lcd]() {
                        EnglishPlayer::GetInstance().Repeat();
                        lcd->UpdateEnglishScreen();
                    });
                    return std::string("重播当前单词。");
                });

    mcp.AddTool(
        "self.english.speak",
        "调整正在进行的英语听写的播报语言，不重新开始、不跳词：0 只报英文，1 只报中文，"
        "2 中英都报。只改后续播报；若用户要马上重听当前词，再调用 repeat。",
        PropertyList({Property("speak", kPropertyTypeInteger, 0, 2)}),
        [lcd](const PropertyList& properties) -> ToolResult {
            const int speak = properties["speak"].value<int>();
            const int mode = speak < 0 ? 0 : (speak % 3);
            Application::GetInstance().Schedule([lcd, mode]() {
                EnglishPlayer::GetInstance().SetSpeak(static_cast<EnglishPlayer::Speak>(mode));
                lcd->UpdateEnglishScreen();
            });
            const char* name = mode == 1 ? "中文" : (mode == 2 ? "中英" : "英文");
            return std::string("播报语言已改为") + name + "。";
        });

    mcp.AddTool("self.english.interval",
                "调整正在进行的英语听写的自动翻页间隔，不重新开始、不跳词、不重播。"
                "seconds 为 0-120 秒；0 表示手动，只有用户说下一个才翻页。",
                PropertyList({Property("seconds", kPropertyTypeInteger, 0, 120)}),
                [lcd](const PropertyList& properties) -> ToolResult {
                    const int seconds = properties["seconds"].value<int>();
                    Application::GetInstance().Schedule([lcd, seconds]() {
                        EnglishPlayer::GetInstance().SetInterval(seconds);
                        lcd->UpdateEnglishScreen();
                    });
                    if (seconds <= 0) {
                        return std::string("已改为手动翻页。");
                    }
                    return "间隔已改为 " + std::to_string(seconds) + " 秒。";
                });

    mcp.AddTool("self.english.stop",
                "结束英语听写并关闭学习页。用户说退出听写、结束听写或关闭学习页时调用；"
                "只调整语言或间隔时不要调用。",
                PropertyList(), [lcd](const PropertyList&) -> ToolResult {
                    Application::GetInstance().Schedule([lcd]() {
                        EnglishPlayer::GetInstance().Stop();
                        lcd->ShowEnglishScreen(false);
                    });
                    return std::string("已结束听写。");
                });
}
