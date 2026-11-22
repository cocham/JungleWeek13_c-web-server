FROM gcc:14-trixie AS build
WORKDIR /src
COPY Makefile .
COPY src/ ./src/
RUN make -j2

FROM debian:trixie-slim
COPY --from=build /src/build/guestbook /src/build/guestbook-backend /usr/local/bin/
WORKDIR /app
COPY public/ ./public/
USER 65534:65534
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/guestbook"]
