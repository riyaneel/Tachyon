FROM gcr.io/oss-fuzz-base/base-builder:v1

RUN apt-get update && apt-get install -y --no-install-recommends ninja-build zip

RUN pip3 install 'cmake>=3.31'

COPY . $SRC/tachyon

WORKDIR $SRC/tachyon

COPY ./.clusterfuzzlite/build.sh $SRC/
