import CoreVideo
import FilterCore
import Foundation
import Metal

public final class CapturedTexture {
    public let pixelBuffer: CVPixelBuffer
    public let backingTexture: CVMetalTexture
    public let texture: MTLTexture

    public init(pixelBuffer: CVPixelBuffer, cache: CVMetalTextureCache) throws {
        guard CVPixelBufferGetPixelFormatType(pixelBuffer) == kCVPixelFormatType_32BGRA,
              CVPixelBufferGetIOSurface(pixelBuffer) != nil else {
            throw RenderingError("Screen capture did not supply an IOSurface-backed SDR BGRA frame.")
        }
        let width = CVPixelBufferGetWidth(pixelBuffer)
        let height = CVPixelBufferGetHeight(pixelBuffer)
        guard width > 0, height > 0 else { throw RenderingError("Screen capture returned an empty frame.") }
        var wrapper: CVMetalTexture?
        let status = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, cache, pixelBuffer, nil,
            .bgra8Unorm, width, height, 0, &wrapper
        )
        guard status == kCVReturnSuccess, let wrapper,
              let texture = CVMetalTextureGetTexture(wrapper) else {
            throw RenderingError("Could not import the captured IOSurface into Metal (code \(status)).")
        }
        self.pixelBuffer = pixelBuffer
        self.backingTexture = wrapper
        self.texture = texture
    }
}

public final class MetalRenderer {
    public let device: MTLDevice
    public let commandQueue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState

    public init(device: MTLDevice) throws {
        self.device = device
        guard let queue = device.makeCommandQueue() else {
            throw RenderingError("Metal could not create a command queue.")
        }
        self.commandQueue = queue
        queue.label = "PaperShade render queue"
        let options = MTLCompileOptions()
        options.fastMathEnabled = false
        options.languageVersion = .version2_4
        let library = try device.makeLibrary(source: Self.shaderSource(), options: options)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.label = "PaperShade single-pass SDR filter"
        descriptor.vertexFunction = library.makeFunction(name: "shadeVertex")
        descriptor.fragmentFunction = library.makeFunction(name: "shadePixel")
        descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        guard descriptor.vertexFunction != nil, descriptor.fragmentFunction != nil else {
            throw RenderingError("The Metal filter entry points are missing.")
        }
        self.pipeline = try device.makeRenderPipelineState(descriptor: descriptor)
    }

    public func makeTextureCache() throws -> CVMetalTextureCache {
        var cache: CVMetalTextureCache?
        let status = CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &cache)
        guard status == kCVReturnSuccess, let cache else {
            throw RenderingError("Metal could not create a capture texture cache (code \(status)).")
        }
        return cache
    }

    public func encode(
        source: MTLTexture,
        destination: MTLTexture,
        parameters: PSFilterParameters,
        commandBuffer: MTLCommandBuffer
    ) throws {
        guard source.width == destination.width, source.height == destination.height,
              source.width > 0, source.height > 0,
              [MTLPixelFormat.bgra8Unorm, .rgba8Unorm].contains(source.pixelFormat),
              destination.pixelFormat == .bgra8Unorm,
              source.sampleCount == 1, destination.sampleCount == 1 else {
            throw RenderingError("Capture and overlay must have identical SDR backing-pixel dimensions.")
        }
        guard (1...4).contains(parameters.pixelSize),
              (0...3).contains(parameters.quantizer), (2...256).contains(parameters.levels),
              parameters.color == 0 || parameters.color == 1,
              parameters.warmthRed.isFinite, parameters.warmthGreen.isFinite, parameters.warmthBlue.isFinite,
              (0...1).contains(parameters.warmthRed),
              (0...1).contains(parameters.warmthGreen),
              (0...1).contains(parameters.warmthBlue),
              MemoryLayout<PSFilterParameters>.size == 64,
              MemoryLayout<PSFilterParameters>.stride == 64 else {
            throw RenderingError("Invalid Metal filter parameters.")
        }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = destination
        pass.colorAttachments[0].loadAction = .dontCare
        pass.colorAttachments[0].storeAction = .store
        guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass) else {
            throw RenderingError("Metal could not create a render encoder.")
        }
        encoder.label = "PaperShade filter"
        encoder.setRenderPipelineState(pipeline)
        encoder.setFragmentTexture(source, index: 0)
        var constants = parameters
        encoder.setFragmentBytes(&constants, length: MemoryLayout<PSFilterParameters>.size, index: 0)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()
    }

    static func shaderSource() throws -> String {
        let ps1 = try CoreParameters.ditherValues(quantizer: 3).map(String.init).joined(separator: ",")
        let bayer = try CoreParameters.ditherValues(quantizer: 2).map(String.init).joined(separator: ",")
        return """
        #include <metal_stdlib>
        using namespace metal;
        #pragma clang fp contract(off)

        // Scalar members preserve the 64-byte C/HLSL ABI; float3 would not.
        struct FilterParameters {
            float red, green, blue, gain;
            float offset;
            int quantizer, levels, pixelSize;
            int color, padding0, padding1, padding2;
            float warmthRed, warmthGreen, warmthBlue, warmthPadding;
        };
        static_assert(sizeof(FilterParameters) == 64, "Filter parameter ABI mismatch");
        constant int ps1[16] = {\(ps1)};
        constant int bayer[16] = {\(bayer)};

        vertex float4 shadeVertex(uint id [[vertex_id]]) {
            float2 p = float2((id << 1) & 2, id & 2);
            return float4(p * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
        }

        fragment float4 shadePixel(
            float4 position [[position]],
            texture2d<float, access::read> desktop [[texture(0)]],
            constant FilterParameters& p [[buffer(0)]]
        ) {
            uint2 pixel = uint2(position.xy);
            float3 source = desktop.read(pixel).rgb;
            float gray = clamp(
                ((source.r * p.red + source.g * p.green) + source.b * p.blue)
                    * p.gain + p.offset, 0.0f, 1.0f);
            float3 color = p.color != 0 ? source : float3(gray);
            uint2 grid = pixel / uint(max(p.pixelSize, 1));
            uint index = (grid.y & 3) * 4 + (grid.x & 3);
            if (p.quantizer == 3) {
                int3 bytes = int3(floor(clamp(color, 0.0f, 1.0f) * 255.0f + 0.5f));
                int3 rgb5 = clamp(bytes + ps1[index], 0, 255) >> 3;
                int3 rgb8 = (rgb5 << 3) | (rgb5 >> 2);
                color = float3(rgb8) / 255.0f;
            }
            if (p.quantizer == 1 || p.quantizer == 2) {
                float threshold = p.quantizer == 2
                    ? (float(bayer[index]) + 0.5f) / 16.0f - 0.5f : 0.0f;
                float steps = float(p.levels - 1);
                color = clamp(floor(color * steps + 0.5f + threshold) / steps, 0.0f, 1.0f);
            }
            return float4(color * float3(p.warmthRed, p.warmthGreen, p.warmthBlue), 1.0f);
        }
        """
    }
}
