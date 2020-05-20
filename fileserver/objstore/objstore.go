// Package objstore provides operations for commit, fs and block objects.
// It is low-level package used by commitmgr, fsmgr, blockmgr packages to access storage.
package objstore

import (
	"io"
)

// ObjectStore is a container to access storage backend
type ObjectStore struct {
	// can be "commit", "fs", or "block"
	ObjType string
	backend storageBackend
}

// storageBackend is the interface implemented by storage backends.
// An object store may have one or multiple storage backends.
type storageBackend interface {
	// Read an object from backend and write the contents into w.
	read(repoID string, objID string, w io.Writer) (err error)
	// Write the contents from r to the object.
	write(repoID string, objID string, r io.Reader, sync bool) (err error)
	// exists checks whether an object exists.
	exists(repoID string, objID string) (res bool, err error)
}

// New returns a new object store for a given type of objects.
// objType can be "commit", "fs", or "block".
func New(seafileConfPath string, seafileDataDir string, objType string) *ObjectStore {
	return nil
}

func (s *ObjectStore) read(repoID string, objID string, w io.Writer) (err error) {
	return s.backend.read(repoID, objID, w)
}

func (s *ObjectStore) write(repoID string, objID string, r io.Reader, sync bool) (err error) {
	return s.backend.write(repoID, objID, r, sync)
}

func (s *ObjectStore) exists(repoID string, objID string) (res bool, err error) {
	return s.backend.exists(repoID, objID)
}
