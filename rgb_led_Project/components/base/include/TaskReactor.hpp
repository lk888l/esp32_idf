/********************************************************************************
  * @file           : TaskReactor.hpp
  * @author         : Luka
  * @brief          : None
  * @attention      : None
  * @date           : 26-3-19
  *******************************************************************************/

//#pragma once
#ifndef TASKREACTOR_HPP
#define TASKREACTOR_HPP

//c++ std library include
#include <functional>
//std library include
#include <vector>
#include <array>
#include <string>
#include <string_view>
//freeRTOS library include
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
//user C++ library include
#include "BasicObject.hpp"

class TaskReactor : public BasicObject {
    using SlotCallback = std::function<void()>;
public:
    TaskReactor(TaskHandle_t taskHandle = nullptr)
        : reactorTask_(taskHandle)
    {
        SlotGroup.fill(nullptr);
    }

    /**
     * @brief
     */
    struct strCMD_t {
        std::string_view command;   // command
        std::string_view args;      // parameter
//        bool hasArgs;               // 是否成功分离出了参数
    };

private:
    std::array<SlotCallback, 32> slots; // 32个Bit对应32个槽
    TaskHandle_t reactorTask_ = nullptr;
    std::array<std::function<void()>, 32> SlotGroup{nullptr};
protected:

    /**
     * @brief
     * @return
     */
    uint32_t allocNotiftBit(){
        for(uint8_t i = 0; i < 32; ++i) {
            if(!SlotGroup[i]) { // 检查 std::function 是否为空
                return (1UL << i);
            }
        }
        return 0;
    }



public:
    /**
     * @brief connect the signal of LKObject.
     * @param sender
     * @param signal
     * @param slot
     * @example     TaskReactor task1;
     * /n           task1.connect();
     */
    template<typename Sender, typename SignalMethod, typename SlotLambda>
    bool connect(Sender* sender, SignalMethod signal, SlotLambda slot) {
        uint32_t bitMask = allocNotiftBit();
        if (bitMask == 0) return false;                             // More than 32 signals failed.
        sender->bindReactor(signal, reactorTask_, bitMask);      // execute LKObject bindReactorBit.
        // Put the slot function into the container
        int bitIndex = __builtin_ctz(bitMask);

        SlotGroup[bitIndex]=[sender, signal, slot]() {
            (sender->*signal)(slot);
        };
        return true;
    }

    /**
     * @brief
     */
    inline void taskLoop(TickType_t xTicksToWait =portMAX_DELAY, const std::function<void()>& func1=nullptr, const std::function<void()>& func2=nullptr) {
        reactorTask_ = xTaskGetCurrentTaskHandle();

        uint32_t notifiedValue = 0;
        TickType_t xLastWakeTime = xTaskGetTickCount();
        while (true) {
            TickType_t xTimeNow = xTaskGetTickCount();
            TickType_t xTicksRemaining = portMAX_DELAY;
            if (func2 && xTicksToWait != portMAX_DELAY) {
                TickType_t elapsed = xTimeNow - xLastWakeTime;
                if (elapsed >= xTicksToWait) {
                    func2(); // 只有时间到了才执行
                    xLastWakeTime = xTaskGetTickCount();
                    elapsed = 0;
                }
                xTicksRemaining = xTicksToWait - elapsed;
            }
            if (xTaskNotifyWait(0x00, 0xFFFFFFFF, &notifiedValue, xTicksRemaining) == pdTRUE){
                for(uint8_t i = 0; notifiedValue > 0 && i < 32; i++) {
                    if(notifiedValue & (1UL << i)) {
                        if (SlotGroup[i]) {
                            SlotGroup[i]();
                        }
                        notifiedValue &= ~(1UL << i); // 清除已处理的位
                    }
                }
                if (func1)  func1();
            }
        }
    }

    static bool parseStrCMD(std::string_view input, strCMD_t &strCmd){
        //Remove leading spaces
        size_t start = input.find_first_not_of(" ");
        if (start == std::string_view::npos) {
            strCmd.command = "";
            strCmd.args = "";
            return false;
        }
        input.remove_prefix(start);
        //Look for the first space.
        size_t spacePos = input.find(' ');
        if (spacePos != std::string_view::npos){
            std::string_view cmd = input.substr(0, spacePos);
            std::string_view args = input.substr(spacePos + 1);
            size_t firstArg = args.find_first_not_of(" ");
            if (firstArg != std::string_view::npos) {
                strCmd.command = cmd;
                strCmd.args    = args;
                return true;
            }
            else{       // 有空格但空格后全是空格
                strCmd.command = input;
                strCmd.args = "";
            }
            return true;
        }
        else{           //纯命令，无参数
            strCmd.command = input;
            strCmd.args = "";
            return true;
        }
    }

    template<typename T>
    static bool parseStrArg(std::string_view &str_arg,T& value){
        // 1. 跳过前导空格
        size_t first = str_arg.find_first_not_of(' ');
        if (first == std::string_view::npos) return false;
        str_arg.remove_prefix(first);

        size_t last = str_arg.find(' ');
        std::string_view token = str_arg.substr(0, last);
        auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (last == std::string_view::npos) str_arg = "";
        else str_arg.remove_prefix(last+1);
        return result.ec == std::errc(); // 返回转换是否成功
    }
};



#endif //TASKREACTOR_HPP