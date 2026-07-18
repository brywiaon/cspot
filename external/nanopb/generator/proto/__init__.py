'''This file automatically rebuilds the proto definitions for Python.'''
from __future__ import absolute_import

import os.path
import sys
import traceback

try:
    import pkg_resources
except ImportError:
    pkg_resources = None

from ._utils import has_grpcio_protoc, invoke_protoc

dirname = os.path.dirname(__file__)
protosrc = os.path.join(dirname, "nanopb.proto")
protodst = os.path.join(dirname, "nanopb_pb2.py")

if os.path.isfile(protosrc):
    src_date = os.path.getmtime(protosrc)
    if not os.path.isfile(protodst) or os.path.getmtime(protodst) < src_date:

        cmd = [
            "protoc",
            "--python_out={}".format(dirname),
            protosrc,
            "-I={}".format(dirname),
        ]

        if has_grpcio_protoc() and pkg_resources is not None:
            # grpcio-tools has an extra CLI argument
            # from grpc.tools.protoc __main__ invocation.
            _builtin_proto_include = pkg_resources.resource_filename('grpc_tools', '_proto')

            cmd.append("-I={}".format(_builtin_proto_include))
        try:
            status = invoke_protoc(argv=cmd)
            if status != 0:
                raise RuntimeError("protoc exited with status {}: {}".format(status, ' '.join(cmd)))
        except Exception:
            sys.stderr.write("Failed to build nanopb_pb2.py: " + ' '.join(cmd) + "\n")
            traceback.print_exc(file=sys.stderr)
            raise
