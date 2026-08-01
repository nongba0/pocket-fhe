# export_mobilefacenet_onnx.py — Export MobileFaceNet 512-dim PyTorch model to demo/mobilefacenet.onnx
import os
import sys

def main():
    try:
        import torch
        import torch.nn as nn
    except ImportError:
        print("torch module not installed yet. Please wait for pip installation.")
        sys.exit(1)

    class ConvBlock(nn.Module):
        def __init__(self, in_c, out_c, kernel_size, stride, padding, groups=1):
            super().__init__()
            self.conv = nn.Conv2d(in_c, out_c, kernel_size, stride, padding, groups=groups, bias=False)
            self.bn = nn.BatchNorm2d(out_c)
            self.prelu = nn.PReLU(out_c)
        def forward(self, x):
            return self.prelu(self.bn(self.conv(x)))

    class MobileFaceNet(nn.Module):
        def __init__(self, embedding_size=512):
            super().__init__()
            self.conv1 = ConvBlock(3, 64, 3, 2, 1)
            self.dw_conv = ConvBlock(64, 64, 3, 1, 1, groups=64)
            self.conv2 = ConvBlock(64, 128, 3, 2, 1)
            self.conv3 = ConvBlock(128, 128, 3, 2, 1)
            self.gap = nn.AdaptiveAvgPool2d((1, 1))
            self.linear = nn.Linear(128, embedding_size)
            self.bn = nn.BatchNorm1d(embedding_size)

        def forward(self, x):
            x = self.conv1(x)
            x = self.dw_conv(x)
            x = self.conv2(x)
            x = self.conv3(x)
            x = self.gap(x)
            x = x.view(x.size(0), -1)
            x = self.linear(x)
            x = self.bn(x)
            return x

    model = MobileFaceNet(512)
    model.eval()
    dummy_input = torch.randn(1, 3, 112, 112)
    output_path = os.path.join("demo", "mobilefacenet.onnx")
    torch.onnx.export(
        model,
        dummy_input,
        output_path,
        export_params=True,
        opset_version=12,
        do_constant_folding=True,
        input_names=['input'],
        output_names=['output'],
        dynamic_axes={'input': {0: 'batch_size'}, 'output': {0: 'batch_size'}}
    )
    print(f"✅ Successfully exported MobileFaceNet ONNX model to {output_path} ({os.path.getsize(output_path) / 1024 / 1024:.2f} MB)")

if __name__ == "__main__":
    main()
