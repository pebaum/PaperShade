import AppKit
import Darwin

@main
struct PaperShadeMain {
    @MainActor
    static func main() {
        let arguments = Array(CommandLine.arguments.dropFirst())
        if arguments == ["--version"] {
            print("PaperShade \(AppController.version)")
            return
        }
        if arguments == ["--help"] {
            print("PaperShade [--version | --smoke-test | --help]")
            return
        }
        let smokeTest = arguments == ["--smoke-test"]
        guard arguments.isEmpty || smokeTest else {
            fputs("Unknown option. Use --help.\n", stderr)
            exit(EXIT_FAILURE)
        }
        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)
        let controller = AppController(smokeTest: smokeTest)
        if smokeTest {
            do {
                try controller.runSmokeTest()
            } catch {
                fputs("PaperShade smoke test failed: \(error.localizedDescription)\n", stderr)
                exit(EXIT_FAILURE)
            }
            return
        }
        app.delegate = controller
        app.run()
        withExtendedLifetime(controller) {}
    }
}
