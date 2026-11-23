// Static hello server on net/http. GET / and POST / return hello\n
// with Content-Type: text/plain. The POST body is read and discarded
// so a keep-alive connection stays framed. Listen backlog is 4096,
// the same as SOMAXCONN and net.core.somaxconn on this host.
// One process, one socket. GOMAXPROCS is the thread cap and is set
// by the bench runner.
package main

import (
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"strconv"
	"syscall"
	"time"
)

func main() {
	port := 18183
	if v := os.Getenv("PORT"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil {
			log.Fatal(err)
		}
		port = n
	}
	fd, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_STREAM, 0)
	if err != nil {
		log.Fatal(err)
	}
	if err = syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1); err != nil {
		log.Fatal(err)
	}
	addr := &syscall.SockaddrInet4{Port: port}
	addr.Addr = [4]byte{127, 0, 0, 1}
	if err = syscall.Bind(fd, addr); err != nil {
		log.Fatal(err)
	}
	if err = syscall.Listen(fd, 4096); err != nil {
		log.Fatal(err)
	}
	f := os.NewFile(uintptr(fd), "listen")
	ln, err := net.FileListener(f)
	if err != nil {
		log.Fatal(err)
	}
	f.Close()
	srv := &http.Server{
		Handler:     http.HandlerFunc(handle),
		IdleTimeout: 60 * time.Second,
	}
	log.Printf("listening 127.0.0.1 %d\n", port)
	log.Fatal(srv.Serve(ln))
}

func handle(w http.ResponseWriter, r *http.Request) {
	_, _ = io.Copy(io.Discard, r.Body)
	_ = r.Body.Close()
	w.Header().Set("Content-Type", "text/plain")
	if r.URL.Path != "/" || (r.Method != http.MethodGet && r.Method != http.MethodPost) {
		if r.Method != http.MethodGet && r.Method != http.MethodPost {
			w.WriteHeader(http.StatusMethodNotAllowed)
			_, _ = w.Write([]byte("method\n"))
			return
		}
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Length", "6")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte("hello\n"))
}
