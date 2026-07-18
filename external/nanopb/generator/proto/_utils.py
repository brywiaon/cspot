import subprocess
import os.path
import sys
import io
import contextlib

def has_grpcio_protoc():
    # type: () -> bool
    """ checks if grpcio-tools protoc is installed"""

    try:
        import grpc_tools.protoc
    except ImportError:
        return False
    return True


def invoke_protoc(argv):
    # type: (list) -> typing.Any
    """
    Invoke protoc.

    This routine will use grpcio-provided protoc if it exists,
    using system-installed protoc as a fallback.

    Args:
        argv: protoc CLI invocation, first item must be 'protoc'
    """

    # Add current directory to include path if nothing else is specified
    if not [x for x in argv if x.startswith('-I')]:
        argv.append("-I.")

    # Add default protoc include paths
    nanopb_include = os.path.dirname(os.path.abspath(__file__))
    argv.append('-I' + nanopb_include)

    protoc_executable = os.environ.get('NANOPB_PROTOC_EXECUTABLE')
    if protoc_executable:
        normalized = os.path.normcase(os.path.normpath(protoc_executable))
        if os.path.basename(normalized) == "protoc" and os.path.exists(os.path.join(os.path.dirname(normalized), "nanopb_generator.py")):
            sys.stderr.write("nanopb invoke_protoc: ignoring self-referential NANOPB_PROTOC_EXECUTABLE=%s\n" % protoc_executable)
        else:
            argv[0] = protoc_executable
            result = subprocess.run(argv, capture_output=True, text=True)
            if result.returncode != 0:
                sys.stderr.write("nanopb invoke_protoc failed command: " + subprocess.list2cmdline(argv) + "\n")
                if result.stderr:
                    sys.stderr.write(result.stderr + "\n")
            return result.returncode

    if has_grpcio_protoc():
        import grpc_tools.protoc as protoc
        try:
            import pkg_resources
            proto_include = pkg_resources.resource_filename('grpc_tools', '_proto')
        except ImportError:
            import grpc_tools
            proto_include = os.path.join(os.path.dirname(grpc_tools.__file__), '_proto')
        argv.append('-I' + proto_include)

        stderr_buffer = io.StringIO()
        with contextlib.redirect_stderr(stderr_buffer):
            status = protoc.main(argv)
        if status != 0:
            sys.stderr.write("nanopb invoke_protoc failed command: " + subprocess.list2cmdline(argv) + "\n")
            stderr_text = stderr_buffer.getvalue()
            if stderr_text:
                sys.stderr.write(stderr_text + "\n")
        return status
    else:
        result = subprocess.run(argv, capture_output=True, text=True)
        if result.returncode != 0:
            sys.stderr.write("nanopb invoke_protoc failed command: " + subprocess.list2cmdline(argv) + "\n")
            if result.stderr:
                sys.stderr.write(result.stderr + "\n")
        return result.returncode
