// macOS read-only check of the native firmware-rendered QR pixels.
// clang -fobjc-arc -framework Foundation -framework Vision decode_qr.m -o decode_qr
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 3) return 2;
        NSURL *url = [NSURL fileURLWithPath:@(argv[1])];
        VNDetectBarcodesRequest *request = [VNDetectBarcodesRequest new];
        request.symbologies = @[VNBarcodeSymbologyQR];
        NSError *error = nil;
        VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithURL:url options:@{}];
        if (![handler performRequests:@[request] error:&error] || error ||
            request.results.count != 1 ||
            ![request.results.firstObject.payloadStringValue isEqualToString:@(argv[2])]) {
            fprintf(stderr, "FAIL: QR did not decode to the unchanged synthetic payload\n");
            return 1;
        }
        puts("PASS: native 240x320 QR decodes to the unchanged synthetic payload");
    }
    return 0;
}
