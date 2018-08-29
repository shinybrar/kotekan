import pytest
import kotekan_runner

# this is the equivalent of the config file for kotekan to run your test
params = {
    'num_elements': 7,
    'num_ev': 0,
    'total_frames': 128,
    'variable_my_process_needs': -1
}

# this runs kotekan and yields the data you want to inspect
@pytest.fixture(scope="module")
def data(tmpdir_factory):

    # keep all the data this test produces in a tmp directory
    tmpdir = tmpdir_factory.mktemp("name_of_the_test_case")

    # you can use FakeVisBuffer to produce fake data
    fakevis_buffer = kotekan_runner.FakeVisBuffer(
        num_frames=params['total_frames'],
        mode='gaussian',
    )

    # DumpVisBuffer can be used to dump data for testing
    dump_buffer = kotekan_runner.DumpVisBuffer(str(tmpdir))

    # KotekanProcessTester is used to run kotekan with your config
    test = kotekan_runner.KotekanProcessTester(
        'processUnderTest', {},
        fakevis_buffer,
        dump_buffer,
        params
)

test.run()

# here the data that the process under test put out is passed on to test the process
yield dump_buffer.load()

# this is the actual test (give a name to it)
def test_<name>(data):

    for frame in data:
        assert (frame.vis == {1,0})