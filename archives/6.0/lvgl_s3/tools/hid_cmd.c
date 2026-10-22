/*
 * hid_cmd.c —— 从 macOS 上通过 USB HID 的 vendor 接口给键盘发一条指令。
 *
 * 为什么需要它：
 *   产品固件跑起来之后，USB 被 TinyUSB 的 HID 键盘占着，macOS 上**看不到
 *   任何串口节点**（/dev/cu.usbmodem* 直接消失）。于是"产品固件在跑"这个
 *   状态下，如果蓝牙再出点问题（比如广播没起来），就**完全没法给键盘发指令**
 *   —— 人的手指够不着的地方全成了死角。
 *   而固件把 USB HID 的 vendor 输出报告也接进了同一个 handleCommand 派发器
 *   （见固件 onHidVendorEvent → handleHidVendorCommands → handleCommand），
 *   所以这条 USB 通道是一条**不依赖蓝牙、不依赖按键**的兜底通道。
 *
 * 固件侧对应：
 *   · 接口：Usage Page 0xFF00 / Usage 0x01（Arduino 的 USBHIDVendor）
 *   · 输出报告：63 字节，无报告 ID（固件会跳过报告尾部的 0 补位）
 *   · 指令以 '\n' 结尾，固件按行解析
 *
 * 用法：
 *   ./hid_cmd "BOOTDL"      # 重启进下载模式
 *   ./hid_cmd "LOG:on"      # 打开蓝牙日志通道
 *   ./hid_cmd --list        # 只列出设备
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <stdio.h>
#include <string.h>

#define VENDOR_VID 0x303A   /* 12346 = YYQ */
#define VENDOR_PID 0x001F   /* 31 */
#define VENDOR_USAGE_PAGE 0xFF00
#define VENDOR_USAGE 0x01
#define VENDOR_REPORT_SIZE 63

static CFNumberRef num(int v) {
    return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &v);
}

/* 只用公开 API：管理器给回来的对象本身就带 UsagePage/Usage 属性，
 * 直接按属性筛出 vendor 那个接口就行，不必去碰 IOHIDServiceRef 那些私有接口。 */
static CFStringRef mkstr(const char *s) {
    return CFStringCreateWithCString(kCFAllocatorDefault, s, kCFStringEncodingUTF8);
}

static int propInt(IOHIDDeviceRef dev, const char *key) {
    CFStringRef k = mkstr(key);
    CFTypeRef t = IOHIDDeviceGetProperty(dev, k);
    CFRelease(k);
    int v = 0;
    if (t) {
        CFNumberGetValue((CFNumberRef)t, kCFNumberSInt32Type, &v);
        CFRelease(t);
    }
    return v;
}

int main(int argc, char **argv) {
    int list_only = (argc > 1 && strcmp(argv[1], "--list") == 0);
    const char *cmd = list_only ? "" : (argc > 1 ? argv[1] : "PING");

    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);

    CFMutableDictionaryRef match = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(match, CFSTR("VendorID"),  num(VENDOR_VID));
    CFDictionarySetValue(match, CFSTR("ProductID"), num(VENDOR_PID));
    IOHIDManagerSetDeviceMatching(mgr, match);
    CFRelease(match);

    IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOReturn or = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    printf("IOHIDManagerOpen → %d %s\n", (int)or,
           or == kIOReturnSuccess ? "✓" : "✗（多半是没有 HID/输入监控权限）");
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);

    CFSetRef devs = IOHIDManagerCopyDevices(mgr);
    if (!devs) { printf("✗ 拿不到设备列表\n"); return 2; }
    CFIndex n = CFSetGetCount(devs);
    printf("匹配到 %ld 个 HID 接口\n", (long)n);

    int sent = 0;
    const void **arr = (const void **)malloc(sizeof(void *) * (n ? n : 1));
    CFSetGetValues(devs, arr);
    for (CFIndex i = 0; i < n; i++) {
        IOHIDDeviceRef dev = (IOHIDDeviceRef)arr[i];
        int pg = propInt(dev, "PrimaryUsagePage");
        int g  = propInt(dev, "PrimaryUsage");
        int pid = propInt(dev, "ProductID");
        int vid = propInt(dev, "VendorID");
        printf("  VID=%04X PID=%04X UsagePage=0x%04X Usage=0x%02X%s\n",
               vid, pid, pg, g,
               (pg == VENDOR_USAGE_PAGE && g == VENDOR_USAGE) ? "   ★ vendor" : "");
        if (pg != VENDOR_USAGE_PAGE || g != VENDOR_USAGE) continue;
        if (list_only) { sent++; continue; }

        unsigned char buf[64];
        memset(buf, 0, sizeof(buf));
        size_t len = strlen(cmd);
        if (len > VENDOR_REPORT_SIZE - 2) len = VENDOR_REPORT_SIZE - 2;
        memcpy(buf, cmd, len);
        buf[len] = '\n';                       /* 固件按行解析 */
        memset(buf + len + 1, 0, VENDOR_REPORT_SIZE - len - 1);

        /* 挨个报告 ID 试：Arduino 的几个 HID 接口用的是不同的报告 ID
         * （键盘=1 / 消费键=4 / 系统键=5 / **厂商=6**），
         * macOS 这个接口要按 ID 选对才能落到厂商那个接口上。
         * 按 IOHIDDeviceSetReport 的约定：设备支持多报告时，
         * **报告 ID 要放在正文第 1 字节**。 */
        int okId = -1;
        for (int rid = 0; rid <= 7; rid++) {
            unsigned char b2[64];
            memset(b2, 0, sizeof(b2));
            b2[0] = (unsigned char)rid;
            memcpy(b2 + 1, cmd, len);
            b2[1 + len] = '\n';
            IOReturn r = IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, rid,
                                              b2, VENDOR_REPORT_SIZE);
            printf("    报告 ID %d → 返回 %d\n", rid, (int)r);
            if (r == kIOReturnSuccess) {
                printf("  报告 ID %d ✓（命中）\n", rid);
                okId = rid;
                break;
            }
        }
        if (okId < 0) {
            printf("  报告 ID 0~7 全部被拒\n");
        } else {
            sent++;
        }
    }
    free(arr);
    CFRelease(devs);
    IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
    CFRelease(mgr);

    if (list_only) return 0;
    if (sent == 0) { printf("✗ 没有成功发出\n"); return 1; }
    printf("✓ 已发出\n");
    return 0;
}
